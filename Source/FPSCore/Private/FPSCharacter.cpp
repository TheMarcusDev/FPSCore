// Copyright 2022 Ellie Kelemen. All Rights Reserved.

#include "FPSCharacter.h"
#include "DrawDebugHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "FPSCharacterController.h"
#include "WeaponBase.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InteractionComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/TimelineComponent.h"
#include "Engine/Engine.h"
#include "TimerManager.h"
#include "Engine/LocalPlayer.h"
#include "Math/UnrealMathUtility.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "Kismet/KismetMathLibrary.h"

// Sets default values
AFPSCharacter::AFPSCharacter()
{
    // Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;

    // Spawning the camera atop the FPS hands mesh
    CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("CameraComp"));
    CameraComponent->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    CameraComponent->bUsePawnControlRotation = true;

    // Spawning the FPS hands mesh component and attaching it to the camera component
    HandsMeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("MeshComp"));
    HandsMeshComp->CastShadow = false;
    HandsMeshComp->bOnlyOwnerSee = true;
    HandsMeshComp->AttachToComponent(CameraComponent, FAttachmentTransformRules::KeepRelativeTransform);

    // Spawning the FPS third person mesh component and attaching it to the capsule component
    ThirdPersonMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ThirdPersonMesh"));
    ThirdPersonMesh->CastShadow = true;
    ThirdPersonMesh->bOwnerNoSee = true;
    ThirdPersonMesh->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);

    // Spawning the FPS shadow mesh component and attaching it to the capsule component
    ShadowMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ShadowMesh"));
    ShadowMesh->CastShadow = true;
    ShadowMesh->bOnlyOwnerSee = true;
    ShadowMesh->bRenderInMainPass = false;
    ShadowMesh->bRenderInDepthPass = true;
    ShadowMesh->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);

    DefaultCapsuleHalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight(); // setting the default height of the capsule
}

// Called when the game starts or when spawned
void AFPSCharacter::BeginPlay()
{
    Super::BeginPlay();

    if (AFPSCharacterController *PlayerController = Cast<AFPSCharacterController>(GetController()))
    {
        SetOwner(PlayerController);
    }

    if (MovementDataMap.Contains(EMovementState::State_Sprint))
    {
        GetCharacterMovement()->MaxWalkSpeed = MovementDataMap[EMovementState::State_Sprint].MaxWalkSpeed;
        UpdateMovementState(EMovementState::State_Idle);
    }
    else
    {
        UE_LOG(LogProfilingDebugging, Error, TEXT("Set up data in MovementDataMap! BeginPlay"));
    }

    DefaultCameraOffset = CameraComponent->GetRelativeLocation().Z; // Setting the default location of the camera

    // Binding a timeline to our vaulting curve
    if (VaultTimelineCurve)
    {
        FOnTimelineFloat TimelineProgress;
        TimelineProgress.BindUFunction(this, FName("TimelineProgress"));
        VaultTimeline.AddInterpFloat(VaultTimelineCurve, TimelineProgress);
    }

    // Obtaining our inventory component and reserving space in memory for our set of weapons
    if (UInventoryComponent *InventoryComp = FindComponentByClass<UInventoryComponent>())
    {
        InventoryComponent = InventoryComp;
        InventoryComponent->GetEquippedWeapons().Reserve(InventoryComponent->GetNumberOfWeaponSlots());
    }

    // Updating the crouched Camera height based on the crouched capsule half height
    CrouchedCameraHeightDelta = CrouchedCapsuleHalfHeight - DefaultCapsuleHalfHeight;

    if (UInventoryComponent *InventoryComp = FindComponentByClass<UInventoryComponent>())
    {
        InventoryComponent = InventoryComp;
        if (AWeaponBase *CurrentWeapon = InventoryComponent->GetCurrentWeapon())
        {
            CurrentWeapon->SetTPAttachment();
        }
    }
}

void AFPSCharacter::PawnClientRestart()
{
    Super::PawnClientRestart();

    // Make sure that we have a valid PlayerController.
    if (const AFPSCharacterController *PlayerController = Cast<AFPSCharacterController>(GetController()))
    {
        // Get the Enhanced Input Local Player Subsystem from the Local Player related to our Player Controller.
        if (UEnhancedInputLocalPlayerSubsystem *Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
        {
            // PawnClientRestart can run more than once in an Actor's lifetime, so start by clearing out any leftover mappings.
            Subsystem->ClearAllMappings();

            // Add each mapping context, along with their priority values. Higher values outprioritize lower values.
            Subsystem->AddMappingContext(BaseMappingContext, BaseMappingPriority);
        }
    }
}

void AFPSCharacter::Move(const FInputActionValue &Value)
{
    // Storing movement vectors for animation manipulation
    ForwardMovement = Value[1];
    RightMovement = Value[0];

    // Moving the player
    if (Value.GetMagnitude() != 0.0f)
    {
        AddMovementInput(GetActorForwardVector(), Value[1]);
        AddMovementInput(GetActorRightVector(), Value[0]);
        if (!bHoldingCrouch)
        {
            if (!bHoldingWalk)
            {
                UpdateMovementState(EMovementState::State_Sprint);
            }
            else
            {
                UpdateMovementState(EMovementState::State_Walk);
            }
        }
    }
    else
    {
        UpdateMovementState(EMovementState::State_Idle);
    }
}

void AFPSCharacter::Look(const FInputActionValue &Value)
{
    // Storing look vectors for animation manipulation
    MouseX = Value[1];
    MouseY = Value[0];

    // Looking around
    AddControllerPitchInput(Value[1] * -1);
    AddControllerYawInput(Value[0]);

    if (InventoryComponent)
    {
        if (Value.GetMagnitude() != 0.0f && InventoryComponent->GetCurrentWeapon())
        {
            // If movement is detected and we have a current weapon, make sure we don't recover the recoil
            InventoryComponent->GetCurrentWeapon()->SetShouldRecover(false);
            InventoryComponent->GetCurrentWeapon()->GetRecoilRecoveryTimeline()->Stop();
        }
    }
}

void AFPSCharacter::ToggleCrouch()
{
    bHoldingCrouch = true;
    if (GetCharacterMovement()->IsMovingOnGround())
    {
        float ForwardVelocity = FVector::DotProduct(GetVelocity(), GetActorForwardVector());
        float RightVelocity = FVector::DotProduct(GetVelocity(), GetActorRightVector());
        if (MovementState == EMovementState::State_Crouch)
        {
            StopCrouch(false);
        }
        else if (MovementState == EMovementState::State_Sprint && !bPerformedSlide && bCanSlide && (ForwardVelocity > MovementDataMap[EMovementState::State_Walk].MaxWalkSpeed || RightVelocity > MovementDataMap[EMovementState::State_Walk].MaxWalkSpeed))
        {
            StartSlide();
        }
        else
        {
            UpdateMovementState(EMovementState::State_Crouch);
        }
    }
    else if (!bPerformedSlide)
    {
        // If we are in the air and have not performed a slide yet
        bWantsToSlide = true;
    }
}

void AFPSCharacter::ReleaseCrouch()
{
    bHoldingCrouch = false;
    bPerformedSlide = false;
    float ForwardVelocity = FVector::DotProduct(GetVelocity(), GetActorForwardVector());
    float RightVelocity = FVector::DotProduct(GetVelocity(), GetActorRightVector());
    if (MovementState == EMovementState::State_Slide)
    {
        StopSlide();
    }
    else if (!bCrouchIsToggle)
    {
        if (MovementState == EMovementState::State_Walk)
        {
            return;
        }
        else if (ForwardVelocity != 0 || RightVelocity != 0)
        {
            UpdateMovementState(EMovementState::State_Sprint);
        }
        else
        {
            UpdateMovementState(EMovementState::State_Idle);
        }
    }
}

void AFPSCharacter::StopCrouch(const bool bToWalk)
{
    if ((MovementState == EMovementState::State_Crouch || MovementState == EMovementState::State_Slide) && HasSpaceToStandUp())
    {
        if (!bToWalk)
        {
            UpdateMovementState(EMovementState::State_Sprint);
        }
        else
        {
            UpdateMovementState(EMovementState::State_Walk);
        }
    }
}

void AFPSCharacter::StartWalk()
{
    bHoldingWalk = true;
    if (!HasSpaceToStandUp())
    {
        return;
    }
    bPerformedSlide = false;
    UpdateMovementState(EMovementState::State_Walk);
    bWantsToWalk = true;
}

void AFPSCharacter::StopWalk()
{
    float ForwardVelocity = FVector::DotProduct(GetVelocity(), GetActorForwardVector());
    float RightVelocity = FVector::DotProduct(GetVelocity(), GetActorRightVector());
    bHoldingWalk = false;
    if (bHoldingCrouch)
    {
        UpdateMovementState(EMovementState::State_Crouch);
    }
    else if (ForwardVelocity != 0 || RightVelocity != 0)
    {
        UpdateMovementState(EMovementState::State_Sprint);
    }
    else
    {
        UpdateMovementState(EMovementState::State_Idle);
    }
    bWantsToWalk = false;
}

void AFPSCharacter::StartSlide()
{
    bIsSliding = true;
    bPerformedSlide = true;
    UpdateMovementState(EMovementState::State_Slide);
    Multi_SlideAnim();
    GetWorldTimerManager().SetTimer(SlideStop, this, &AFPSCharacter::StopSlide, SlideTime, false, SlideTime);
    GetWorldTimerManager().SetTimer(SlideTimeOutHandler, this, &AFPSCharacter::TimeOutSlide, SlideTimeOut, false, SlideTimeOut);
    bCanSlide = false;
}

void AFPSCharacter::Multi_SlideAnim_Implementation()
{
    HandsMeshComp->GetAnimInstance()->Montage_Play(SlideMontage, 1.0f);
    ThirdPersonMesh->GetAnimInstance()->Montage_Play(SlideMontage, 1.0f);
}

void AFPSCharacter::TimeOutSlide()
{
    bCanSlide = true;
    GetWorldTimerManager().ClearTimer(SlideTimeOutHandler);
}

void AFPSCharacter::StopSlide()
{
    if (MovementState == EMovementState::State_Slide && FloorAngle > SlideContinueAngle)
    {
        bIsSliding = false;

        if (!HasSpaceToStandUp())
        {
            UpdateMovementState(EMovementState::State_Crouch);
        }
        else if (bWantsToWalk)
        {
            StopCrouch(true);
        }
        else if (bHoldingCrouch)
        {
            UpdateMovementState(EMovementState::State_Crouch);
        }
        else
        {
            UpdateMovementState(EMovementState::State_Sprint);
        }
        GetWorldTimerManager().ClearTimer(SlideStop);
    }
    else if (FloorAngle < -SlideContinueAngle)
    {
        GetWorldTimerManager().SetTimer(SlideStop, this, &AFPSCharacter::StopSlide, 0.1f, false, 0.1f);
    }
}

void AFPSCharacter::StartAds()
{
    bWantsToAim = true;
}

void AFPSCharacter::StopAds()
{
    bWantsToAim = false;
}

void AFPSCharacter::CheckVault()
{
    if (!bCanVault)
        return;

    float ForwardVelocity = FVector::DotProduct(GetVelocity(), GetActorForwardVector());
    if (!(ForwardVelocity > 0 && !bIsVaulting && GetCharacterMovement()->IsFalling()))
        return;

    // Store these for future use.
    FVector ColliderLocation = GetCapsuleComponent()->GetComponentLocation();
    FRotator ColliderRotation = GetCapsuleComponent()->GetComponentRotation();
    FVector StartLocation = ColliderLocation;
    FVector EndLocation = ColliderLocation + UKismetMathLibrary::GetForwardVector(ColliderRotation) * 75;
    if (bDrawDebug)
    {
        DrawDebugCapsule(GetWorld(), StartLocation, 50, 30, FQuat::Identity, FColor::Red);
    }

    FCollisionQueryParams TraceParams;
    TraceParams.bTraceComplex = true;
    TraceParams.AddIgnoredActor(this);

    // Checking if we are near a wall
    if (!GetWorld()->SweepSingleByChannel(MantleHit, StartLocation, EndLocation, FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeCapsule(30, 50), TraceParams))
        return;
    if (!MantleHit.bBlockingHit)
        return;

    FVector ForwardImpactPoint = MantleHit.ImpactPoint;
    FVector ForwardImpactNormal = MantleHit.ImpactNormal;
    FVector CapsuleLocation = ForwardImpactPoint;
    CapsuleLocation.Z = ColliderLocation.Z;
    CapsuleLocation += ForwardImpactNormal * -15;
    StartLocation = CapsuleLocation;
    StartLocation.Z += 100;
    EndLocation = CapsuleLocation;

    // Checking if we can stand up on the wall that we've hit
    if (!GetWorld()->SweepSingleByChannel(MantleHit, StartLocation, EndLocation, FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(1), TraceParams))
        return;
    if (!GetCharacterMovement()->IsWalkable(MantleHit))
        return;

    FVector SecondaryVaultStartLocation = MantleHit.ImpactPoint;
    SecondaryVaultStartLocation.Z += 5;
    FVector SecondaryVaultEndLocation = SecondaryVaultStartLocation;
    SecondaryVaultEndLocation.Z = 0;
    FVector SecondaryVaultHeightCheckLocation = SecondaryVaultStartLocation;
    SecondaryVaultHeightCheckLocation.Z += VaultSpaceHeight;

    if (bDrawDebug)
    {
        DrawDebugSphere(GetWorld(), SecondaryVaultStartLocation, 10, 8, FColor::Orange);
    }

    float InitialTraceHeight = 0;
    float PreviousTraceHeight = 0;
    float CurrentTraceHeight = 0;
    bool bInitialSwitch = false;
    bool bVaultFailed = true;

    int i;

    FVector ForwardAddition = UKismetMathLibrary::GetForwardVector(ColliderRotation) * 5;
    float CalculationHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2;
    float ScaledCapsuleWithoutHemisphere = GetCapsuleComponent()->GetScaledCapsuleHalfHeight_WithoutHemisphere();

    // Tracing downwards VaultTraceAmount times and looking for a significant change in height followed by a space large enough to stand
    for (i = 0; i <= VaultTraceAmount; i++)
    {
        SecondaryVaultStartLocation += ForwardAddition;
        SecondaryVaultEndLocation += ForwardAddition;
        SecondaryVaultHeightCheckLocation += ForwardAddition;
        bVaultFailed = true;
        if (!GetWorld()->LineTraceSingleByChannel(VaultHit, SecondaryVaultStartLocation, SecondaryVaultEndLocation, ECC_WorldStatic, TraceParams))
            continue;
        if (bDrawDebug)
        {
            DrawDebugLine(GetWorld(), SecondaryVaultStartLocation, VaultHit.ImpactPoint, FColor::Red, false, 10.0f, 0.0f, 2.0f);
        }

        if (bDrawDebug)
        {
            DrawDebugLine(GetWorld(), SecondaryVaultStartLocation, SecondaryVaultHeightCheckLocation, FColor::Green, false, 10.0f, 0.0f, 2.0f);
        }
        if (GetWorld()->LineTraceSingleByChannel(VaultHeightHit, SecondaryVaultStartLocation, SecondaryVaultHeightCheckLocation, ECC_WorldStatic, TraceParams))
            break;

        float TraceLength = SecondaryVaultStartLocation.Z - VaultHit.ImpactPoint.Z;
        if (!bInitialSwitch)
        {
            InitialTraceHeight = TraceLength;
            bInitialSwitch = true;
        }

        PreviousTraceHeight = CurrentTraceHeight;
        CurrentTraceHeight = TraceLength;
        if (!(!(FMath::IsNearlyEqual(CurrentTraceHeight, InitialTraceHeight, 20.0f)) && CurrentTraceHeight < MaxMantleHeight))
            continue;

        if (!FMath::IsNearlyEqual(PreviousTraceHeight, CurrentTraceHeight, 3.0f))
            continue;

        FVector DownTracePoint = VaultHit.Location;
        DownTracePoint.Z = VaultHit.ImpactPoint.Z;

        FVector CalculationVector = FVector::ZeroVector;
        CalculationVector.Z = CalculationHeight;
        DownTracePoint += CalculationVector;
        StartLocation = DownTracePoint;
        StartLocation.Z += ScaledCapsuleWithoutHemisphere;
        EndLocation = DownTracePoint;
        EndLocation.Z -= ScaledCapsuleWithoutHemisphere;

        if (bDrawDebug)
        {
            DrawDebugCapsule(GetWorld(), StartLocation, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), GetCapsuleComponent()->GetUnscaledCapsuleRadius(), FQuat::Identity, FColor::Green, false, 10.0f);
        }
        if (GetWorld()->SweepSingleByChannel(VaultHit, StartLocation, EndLocation, FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(GetCapsuleComponent()->GetUnscaledCapsuleRadius()), TraceParams))
            continue;

        // If we find such a location, break the loop and vault
        ForwardImpactNormal.X -= 1;
        ForwardImpactNormal.Y -= 1;
        VaultTargetLocation = FTransform(UKismetMathLibrary::MakeRotFromX(ForwardImpactNormal), DownTracePoint);
        bIsVaulting = true;
        Vault(VaultTargetLocation);
        bVaultFailed = false;
        break;
    }

    if (!bVaultFailed)
        return;

    // If the vault has failed (there is no space or the surface is too high), we proceed to perform the mantle logic

    FVector DownTracePoint = MantleHit.Location;
    DownTracePoint.Z = MantleHit.ImpactPoint.Z;

    FVector CalculationVector = FVector::ZeroVector;
    CalculationVector.Z = GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2;
    DownTracePoint += CalculationVector;
    StartLocation = DownTracePoint;
    StartLocation.Z += GetCapsuleComponent()->GetScaledCapsuleHalfHeight_WithoutHemisphere();
    EndLocation = DownTracePoint;
    EndLocation.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight_WithoutHemisphere();

    // Looking for a safe place to mantle to
    if (GetWorld()->SweepSingleByChannel(MantleHit, StartLocation, EndLocation, FQuat::Identity, ECC_WorldStatic,
                                         FCollisionShape::MakeSphere(GetCapsuleComponent()->GetUnscaledCapsuleRadius()),
                                         TraceParams))
        return;

    ForwardImpactNormal.X -= 1;
    ForwardImpactNormal.Y -= 1;
    VaultTargetLocation = FTransform(UKismetMathLibrary::MakeRotFromX(ForwardImpactNormal), DownTracePoint);
    bIsVaulting = true;

    // Calling vault with our mantle target point
    Vault(VaultTargetLocation);
}

// Progresses the timeline that is used to vault the character
void AFPSCharacter::TimelineProgress(const float Value)
{
    const FVector NewLocation = FMath::Lerp(VaultStartLocation.GetLocation(), VaultEndLocation.GetLocation(), Value);
    SetActorLocation(NewLocation);
    if (Value == 1)
    {
        bIsVaulting = false;
        if (!bWantsToWalk)
        {
            UpdateMovementState(EMovementState::State_Sprint);
        }
        else
        {
            UpdateMovementState(EMovementState::State_Walk);
        }
    }
}

void AFPSCharacter::CheckGroundAngle(float DeltaTime)
{
    FCollisionQueryParams TraceParams;
    TraceParams.bTraceComplex = true;
    TraceParams.AddIgnoredActor(this);

    // Determines the angle of the floor from the vector of a hit line trace
    FVector CapsuleHeight = GetCapsuleComponent()->GetComponentLocation();
    CapsuleHeight.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const FVector AngleStartTrace = CapsuleHeight;
    FVector AngleEndTrace = AngleStartTrace;
    AngleEndTrace.Z -= 50;
    if (GetWorld()->LineTraceSingleByChannel(AngleHit, AngleStartTrace, AngleEndTrace, ECC_WorldStatic, TraceParams))
    {
        const FVector FloorVector = AngleHit.ImpactNormal;
        const FRotator FinalRotation = UKismetMathLibrary::MakeRotFromZX(FloorVector, GetActorForwardVector());
        FloorAngle = FinalRotation.Pitch;
        if (bDrawDebug)
        {
            GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, FString::Printf(TEXT("Current floor angle = %f"), FloorAngle), true);
        }
    }
}

float AFPSCharacter::CheckRelativeMovementAngle(const float DeltaTime) const
{
    const FVector MovementVector = GetVelocity();
    const FRotator MovementRotator = GetActorRotation();
    const FVector RelativeMovementVector = MovementRotator.UnrotateVector(MovementVector);

    if (bDrawDebug)
    {
        GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Orange, FString::SanitizeFloat(FMath::Abs(RelativeMovementVector.HeadingAngle() * (180 / PI))));
    }

    return FMath::Abs(RelativeMovementVector.HeadingAngle());
}

bool AFPSCharacter::HasSpaceToStandUp()
{
    FVector CenterVector = GetActorLocation();
    CenterVector.Z += 44;

    const float CollisionCapsuleHeight = DefaultCapsuleHalfHeight - 17.0f;

    // Check to see if a capsule collision collides with the environment, if yes, we don't have space to stand up
    const FCollisionShape CollisionCapsule = FCollisionShape::MakeCapsule(30.0f, CollisionCapsuleHeight);

    if (bDrawDebug)
    {
        DrawDebugCapsule(GetWorld(), CenterVector, CollisionCapsuleHeight, 30.0f, FQuat::Identity, FColor::Red, false, 5.0f, 0, 3);
    }

    FCollisionQueryParams QueryParams;
    QueryParams.AddIgnoredActor(this);

    if (GetWorld()->SweepSingleByChannel(StandUpHit, CenterVector, CenterVector, FQuat::Identity, ECC_WorldStatic, CollisionCapsule, QueryParams))
    {
        /* confetti or smth idk */
        if (bDrawDebug)
        {
            GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Red, "Stand up trace returned hit", true);
        }
        return false;
    }

    return true;
}

void AFPSCharacter::Vault(const FTransform TargetTransform)
{
    // Updating our target location and playing the vault timeline from start
    VaultStartLocation = GetActorTransform();
    VaultEndLocation = TargetTransform;
    UpdateMovementState(EMovementState::State_Vault);
    if (VaultMontage)
    {
        HandsMeshComp->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
        ThirdPersonMesh->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
        if (!IsNetMode(NM_DedicatedServer) && !IsNetMode(NM_ListenServer))
        {
            Server_Vault(TargetTransform);
        }
        Multi_Vault(TargetTransform);
    }
    VaultTimeline.PlayFromStart();
}

void AFPSCharacter::Server_Vault_Implementation(const FTransform TargetTransform)
{
    // Updating our target location and playing the vault timeline from start
    VaultStartLocation = GetActorTransform();
    VaultEndLocation = TargetTransform;
    UpdateMovementState(EMovementState::State_Vault);
    if (VaultMontage)
    {
        HandsMeshComp->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
        ThirdPersonMesh->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
    }
    VaultTimeline.PlayFromStart();
}

void AFPSCharacter::Multi_Vault_Implementation(const FTransform TargetTransform)
{
    // Updating our target location and playing the vault timeline from start
    VaultStartLocation = GetActorTransform();
    VaultEndLocation = TargetTransform;
    UpdateMovementState(EMovementState::State_Vault);
    if (VaultMontage)
    {
        HandsMeshComp->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
        ThirdPersonMesh->GetAnimInstance()->Montage_Play(VaultMontage, 1.0f);
    }
    VaultTimeline.PlayFromStart();
}

void AFPSCharacter::Multi_UpdateMovementState_Implementation(const EMovementState NewMovementState)
{
    // Clearing sprinting and crouching flags
    bIsSprinting = false;
    bIsCrouching = false;
    bIsWalking = false;
    bIsVaulting = false;
    bIsSliding = false;

    // Updating the movement state
    MovementState = NewMovementState;

    if (MovementDataMap.Contains(MovementState))
    {
        // Updating CharacterMovementComponent variables based on movement state
        if (InventoryComponent)
        {
            if (InventoryComponent->GetCurrentWeapon())
            {
                // Check if AnimationWaitDelay timer is active and get its remaining time
                float RemainingTime = 0.0f;
                ActiveTimer = InventoryComponent->GetCurrentWeapon()->GetAnimationWaitDelay();
                RemainingTime = GetWorld()->GetTimerManager().GetTimerRemaining(ActiveTimer);
                if (GetWorld()->GetTimerManager().IsTimerActive(ActiveTimer))
                {
                    FTimerDelegate TimerDelegate = FTimerDelegate::CreateUObject(this, &AFPSCharacter::EnableWeaponFire);
                    if (!GetWorld()->GetTimerManager().IsTimerActive(WaitForAnim))
                    {
                        GetWorld()->GetTimerManager().ClearTimer(WaitForAnim);
                        GetWorld()->GetTimerManager().SetTimer(WaitForAnim, TimerDelegate, RemainingTime, false);
                    }
                }
                else
                {
                    InventoryComponent->GetCurrentWeapon()->SetCanFire(MovementDataMap[MovementState].bCanFire);
                }
                InventoryComponent->GetCurrentWeapon()->SetCanReload(MovementDataMap[MovementState].bCanReload);
            }
        }
        GetCharacterMovement()->MaxAcceleration = MovementDataMap[MovementState].MaxAcceleration;
        GetCharacterMovement()->BrakingDecelerationWalking = MovementDataMap[MovementState].BreakingDecelerationWalking;
        GetCharacterMovement()->GroundFriction = MovementDataMap[MovementState].GroundFriction;
        GetCharacterMovement()->MaxWalkSpeed = MovementDataMap[MovementState].MaxWalkSpeed;
    }

    // Updating sprinting and crouching flags
    if (MovementState == EMovementState::State_Crouch)
    {
        bIsCrouching = true;
    }
    if (MovementState == EMovementState::State_Sprint)
    {
        bIsSprinting = true;
    }
    if (MovementState == EMovementState::State_Walk)
    {
        bIsWalking = true;
    }
    if (MovementState == EMovementState::State_Vault)
    {
        bIsVaulting = true;
    }
    if (MovementState == EMovementState::State_Slide)
    {
        bIsSliding = true;
    }
}

void AFPSCharacter::Server_UpdateMovementState_Implementation(const EMovementState NewMovementState)
{
    // Clearing sprinting and crouching flags
    bIsSprinting = false;
    bIsCrouching = false;
    bIsWalking = false;
    bIsVaulting = false;
    bIsSliding = false;

    // Updating the movement state
    MovementState = NewMovementState;

    if (MovementDataMap.Contains(MovementState))
    {
        // Updating CharacterMovementComponent variables based on movement state
        if (InventoryComponent)
        {
            if (InventoryComponent->GetCurrentWeapon())
            {
                // Check if AnimationWaitDelay timer is active and get its remaining time
                float RemainingTime = 0.0f;
                ActiveTimer = InventoryComponent->GetCurrentWeapon()->GetAnimationWaitDelay();
                RemainingTime = GetWorld()->GetTimerManager().GetTimerRemaining(ActiveTimer);
                if (GetWorld()->GetTimerManager().IsTimerActive(ActiveTimer))
                {
                    FTimerDelegate TimerDelegate = FTimerDelegate::CreateUObject(this, &AFPSCharacter::EnableWeaponFire);
                    if (!GetWorld()->GetTimerManager().IsTimerActive(WaitForAnim))
                    {
                        GetWorld()->GetTimerManager().ClearTimer(WaitForAnim);
                        GetWorld()->GetTimerManager().SetTimer(WaitForAnim, TimerDelegate, RemainingTime, false);
                    }
                }
                else
                {
                    InventoryComponent->GetCurrentWeapon()->SetCanFire(MovementDataMap[MovementState].bCanFire);
                }
                InventoryComponent->GetCurrentWeapon()->SetCanReload(MovementDataMap[MovementState].bCanReload);
            }
        }
        GetCharacterMovement()->MaxAcceleration = MovementDataMap[MovementState].MaxAcceleration;
        GetCharacterMovement()->BrakingDecelerationWalking = MovementDataMap[MovementState].BreakingDecelerationWalking;
        GetCharacterMovement()->GroundFriction = MovementDataMap[MovementState].GroundFriction;
        GetCharacterMovement()->MaxWalkSpeed = MovementDataMap[MovementState].MaxWalkSpeed;
    }

    // Updating sprinting and crouching flags
    if (MovementState == EMovementState::State_Crouch)
    {
        bIsCrouching = true;
    }
    if (MovementState == EMovementState::State_Sprint)
    {
        bIsSprinting = true;
    }
    if (MovementState == EMovementState::State_Walk)
    {
        bIsWalking = true;
    }
    if (MovementState == EMovementState::State_Vault)
    {
        bIsVaulting = true;
    }
    if (MovementState == EMovementState::State_Slide)
    {
        bIsSliding = true;
    }
    Multi_UpdateMovementState(NewMovementState);
}

// Function that determines the player's maximum speed and other related variables based on movement state
void AFPSCharacter::UpdateMovementState(const EMovementState NewMovementState)
{
    // Clearing sprinting and crouching flags
    bIsSprinting = false;
    bIsCrouching = false;
    bIsWalking = false;
    bIsVaulting = false;
    bIsSliding = false;

    // Updating the movement state
    MovementState = NewMovementState;

    if (MovementDataMap.Contains(MovementState))
    {
        // Updating CharacterMovementComponent variables based on movement state
        if (InventoryComponent)
        {
            if (InventoryComponent->GetCurrentWeapon())
            {
                // Check if AnimationWaitDelay timer is active and get its remaining time
                float RemainingTime = 0.0f;
                ActiveTimer = InventoryComponent->GetCurrentWeapon()->GetAnimationWaitDelay();
                RemainingTime = GetWorld()->GetTimerManager().GetTimerRemaining(ActiveTimer);
                if (GetWorld()->GetTimerManager().IsTimerActive(ActiveTimer))
                {
                    FTimerDelegate TimerDelegate = FTimerDelegate::CreateUObject(this, &AFPSCharacter::EnableWeaponFire);
                    if (!GetWorld()->GetTimerManager().IsTimerActive(WaitForAnim))
                    {
                        GetWorld()->GetTimerManager().ClearTimer(WaitForAnim);
                        GetWorld()->GetTimerManager().SetTimer(WaitForAnim, TimerDelegate, RemainingTime, false);
                    }
                }
                else
                {
                    InventoryComponent->GetCurrentWeapon()->SetCanFire(MovementDataMap[MovementState].bCanFire);
                }
                InventoryComponent->GetCurrentWeapon()->SetCanReload(MovementDataMap[MovementState].bCanReload);
            }
        }
        GetCharacterMovement()->MaxAcceleration = MovementDataMap[MovementState].MaxAcceleration;
        GetCharacterMovement()->BrakingDecelerationWalking = MovementDataMap[MovementState].BreakingDecelerationWalking;
        GetCharacterMovement()->GroundFriction = MovementDataMap[MovementState].GroundFriction;
        GetCharacterMovement()->MaxWalkSpeed = MovementDataMap[MovementState].MaxWalkSpeed;
    }

    // Updating sprinting and crouching flags
    if (MovementState == EMovementState::State_Crouch)
    {
        bIsCrouching = true;
    }
    if (MovementState == EMovementState::State_Sprint)
    {
        bIsSprinting = true;
    }
    if (MovementState == EMovementState::State_Walk)
    {
        bIsWalking = true;
    }
    if (MovementState == EMovementState::State_Vault)
    {
        bIsVaulting = true;
    }
    if (MovementState == EMovementState::State_Slide)
    {
        bIsSliding = true;
    }
    if (!HasAuthority())
    {
        Server_UpdateMovementState(NewMovementState);
    }
    else
    {
        Multi_UpdateMovementState(NewMovementState);
    }
}

void AFPSCharacter::EnableWeaponFire()
{
    if (InventoryComponent && InventoryComponent->GetCurrentWeapon())
    {
        InventoryComponent->GetCurrentWeapon()->SetCanFire(MovementDataMap[MovementState].bCanFire);
    }
}

void AFPSCharacter::Server_VaultTimelineTick_Implementation(const float DeltaTime)
{
    VaultTimeline.TickTimeline(DeltaTime);
}

void AFPSCharacter::Multi_VaultTimelineTick_Implementation(const float DeltaTime)
{
    VaultTimeline.TickTimeline(DeltaTime);
}

// Called every frame
void AFPSCharacter::Tick(const float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Timeline tick
    VaultTimeline.TickTimeline(DeltaTime);
    Multi_VaultTimelineTick(DeltaTime);
    if (!IsNetMode(NM_DedicatedServer) && !IsNetMode(NM_ListenServer))
    {
        Server_VaultTimelineTick(DeltaTime);
    }

    // Crouching
    // Sets the new Target Half Height based on whether the player is crouching or standing
    const float TargetHalfHeight = (MovementState == EMovementState::State_Crouch || MovementState == EMovementState::State_Slide) ? CrouchedCapsuleHalfHeight : DefaultCapsuleHalfHeight;
    const float CameraTargetOffset = (MovementState == EMovementState::State_Crouch || MovementState == EMovementState::State_Slide) ? DefaultCameraOffset + CrouchedCameraHeightDelta : DefaultCameraOffset;
    // Interpolates between the current height and the target height
    const float NewHalfHeight = FMath::FInterpTo(GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), TargetHalfHeight, DeltaTime, CrouchSpeed);
    const float NewLocation = FMath::FInterpTo(CurrentCameraOffset, CameraTargetOffset, DeltaTime, CrouchSpeed);
    CurrentCameraOffset = NewLocation;
    // Sets the half height of the capsule component to the new interpolated half height
    GetCapsuleComponent()->SetCapsuleHalfHeight(NewHalfHeight);
    FVector NewCameraLocation = CameraComponent->GetRelativeLocation();
    NewCameraLocation.Z = NewLocation;
    CameraComponent->SetRelativeLocation(NewCameraLocation);

    if (bRestrictSprintAngle)
    {
        const float CurrentRelativeMovementAngle = CheckRelativeMovementAngle(DeltaTime);

        // Sprinting
        if (CurrentRelativeMovementAngle > (SprintAngleLimit * (PI / 180)) && MovementState == EMovementState::State_Sprint)
        {
            UpdateMovementState(EMovementState::State_Walk);
            bRestrictingSprint = true;
        }
        else if (CurrentRelativeMovementAngle < (SprintAngleLimit * (PI / 180)) && bRestrictingSprint && !bWantsToWalk)
        {
            UpdateMovementState(EMovementState::State_Sprint);
            bRestrictingSprint = false;
        }
    }

    // FOV adjustments
    if (MovementDataMap.Contains(EMovementState::State_Walk))
    {
        float TargetFOV = ((MovementState == EMovementState::State_Sprint || MovementState == EMovementState::State_Slide) && GetVelocity().Size() > MovementDataMap[EMovementState::State_Walk].MaxWalkSpeed) ? BaseFOV + FOVOffset + SpeedFOVChange : BaseFOV + FOVOffset;
        if (InventoryComponent)
        {
            if (InventoryComponent->GetCurrentWeapon())
            {
                if (bIsAiming && InventoryComponent->GetCurrentWeapon()->GetStaticWeaponData()->bAimingFOV && !InventoryComponent->GetCurrentWeapon()->IsReloading())
                {
                    TargetFOV = BaseFOV + FOVOffset - InventoryComponent->GetCurrentWeapon()->GetStaticWeaponData()->AimingFOVChange;
                }
            }
        }

        // Interpolates between current fov and target fov
        const float InFieldOfView = FMath::FInterpTo(CameraComponent->FieldOfView, TargetFOV, DeltaTime, FOVChangeSpeed);
        // Sets the new camera FOV
        CameraComponent->SetFieldOfView(InFieldOfView);
    }
    else
    {
        UE_LOG(LogProfilingDebugging, Error, TEXT("Set up data in MovementDataMap! Fov adjustments"))
    }

    // Continuous aiming check (so that you don't have to re-press the ADS button every time you jump/sprint/reload/etc)
    if (bWantsToAim == true && MovementState != EMovementState::State_Slide)
    {
        bIsAiming = true;
    }
    else
    {
        bIsAiming = false;
    }

    // Slide performed check, so that if the player is in the air and presses the slide key, they slide when they land
    if (GetCharacterMovement()->IsMovingOnGround() && !bPerformedSlide && bWantsToSlide)
    {
        float ForwardVelocity = FVector::DotProduct(GetVelocity(), GetActorForwardVector());
        float RightVelocity = FVector::DotProduct(GetVelocity(), GetActorRightVector());
        if (ForwardVelocity > MovementDataMap[EMovementState::State_Walk].MaxWalkSpeed || RightVelocity > MovementDataMap[EMovementState::State_Walk].MaxWalkSpeed)
        {
            StartSlide();
            bWantsToSlide = false;
        }
    }

    // Checks whether we can vault every frame
    CheckVault();

    // Checks the floor angle to determine whether we should keep sliding or not
    CheckGroundAngle(DeltaTime);

    if (bDrawDebug)
    {
        if (InventoryComponent)
        {
            for (int Index = 0; Index < InventoryComponent->GetNumberOfWeaponSlots(); Index++)
            {
                if (InventoryComponent->GetEquippedWeapons().Contains(Index))
                {
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, FString::SanitizeFloat(InventoryComponent->GetEquippedWeapons()[Index]->GetRuntimeWeaponData()->ClipSize));
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, FString::SanitizeFloat(InventoryComponent->GetEquippedWeapons()[Index]->GetRuntimeWeaponData()->ClipCapacity));
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, FString::SanitizeFloat(InventoryComponent->GetEquippedWeapons()[Index]->GetRuntimeWeaponData()->WeaponHealth));
                }
                else
                {
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, TEXT("No Weapon Found"));
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, TEXT("No Weapon Found"));
                    GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, TEXT("No Weapon Found"));
                }
                GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Red, FString::FromInt(Index));
            }
        }
    }
}

// Called to bind functionality to input
void AFPSCharacter::SetupPlayerInputComponent(UInputComponent *PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    // Make sure that we are using a UEnhancedInputComponent; if not, the project is not configured correctly.
    if (UEnhancedInputComponent *PlayerEnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        if (UInteractionComponent *InteractionComponent = FindComponentByClass<UInteractionComponent>())
        {
            InteractionComponent->InteractAction = InteractAction;
            InteractionComponent->SetupInputComponent(PlayerEnhancedInputComponent);
        }

        if (UInventoryComponent *InventoryComp = FindComponentByClass<UInventoryComponent>())
        {
            InventoryComp->FiringAction = FiringAction;
            InventoryComp->PrimaryWeaponAction = PrimaryWeaponAction;
            InventoryComp->SecondaryWeaponAction = SecondaryWeaponAction;
            InventoryComp->ReloadAction = ReloadAction;
            InventoryComp->ScrollAction = ScrollAction;
            InventoryComp->InspectWeaponAction = InspectWeaponAction;

            InventoryComp->SetupInputComponent(PlayerEnhancedInputComponent);
        }

        if (JumpAction)
        {
            // Jumping
            PlayerEnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AFPSCharacter::Jump);
        }

        if (WalkAction)
        {
            // Walking
            PlayerEnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Started, this, &AFPSCharacter::StartWalk);
            PlayerEnhancedInputComponent->BindAction(WalkAction, ETriggerEvent::Completed, this, &AFPSCharacter::StopWalk);
        }

        if (MovementAction)
        {
            // Move forward/back + left/right inputs
            PlayerEnhancedInputComponent->BindAction(MovementAction, ETriggerEvent::Triggered, this, &AFPSCharacter::Move);
        }

        if (LookAction)
        {
            // Look up/down + left/right
            PlayerEnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFPSCharacter::Look);
        }

        if (AimAction)
        {
            // Aiming
            PlayerEnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &AFPSCharacter::StartAds);
            PlayerEnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Completed, this, &AFPSCharacter::StopAds);
        }

        if (CrouchAction)
        {
            // Crouching
            PlayerEnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Started, this, &AFPSCharacter::ToggleCrouch);
            PlayerEnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Completed, this, &AFPSCharacter::ReleaseCrouch);
        }
        if (FiringAction)
        {
            // Firing
            PlayerEnhancedInputComponent->BindAction(FiringAction, ETriggerEvent::Started, this, &AFPSCharacter::Fire);
            PlayerEnhancedInputComponent->BindAction(FiringAction, ETriggerEvent::Completed, this, &AFPSCharacter::StopFire);
        }
        if (ReloadAction)
        {
            // Reloading
            PlayerEnhancedInputComponent->BindAction(ReloadAction, ETriggerEvent::Started, this, &AFPSCharacter::Reload);
        }
    }
}

void AFPSCharacter::Fire()
{
    if (HasAuthority())
    {
        if (InventoryComponent->GetCurrentWeapon())
        {
            FVector CameraLocation = GetCameraComponent()->GetComponentLocation();
            FRotator CameraRotation = GetCameraComponent()->GetComponentRotation();
            InventoryComponent->GetCurrentWeapon()->StartFire(CameraLocation, CameraRotation);
        }
    }
    else
    {
        FVector CameraLocation = GetCameraComponent()->GetComponentLocation();
        FRotator CameraRotation = GetCameraComponent()->GetComponentRotation();
        Server_Fire(CameraLocation, CameraRotation);
    }
}

bool AFPSCharacter::Server_Fire_Validate(FVector CameraLocation, FRotator CameraRotation)
{
    return true;
}

void AFPSCharacter::Server_Fire_Implementation(FVector CameraLocation, FRotator CameraRotation)
{
    if (InventoryComponent->GetCurrentWeapon())
    {
        InventoryComponent->GetCurrentWeapon()->StartFire(CameraLocation, CameraRotation);
    }
}

void AFPSCharacter::StopFire()
{
    if (HasAuthority())
    {
        if (InventoryComponent->GetCurrentWeapon())
        {
            InventoryComponent->GetCurrentWeapon()->StopFire();
        }
    }
    else
    {
        Server_StopFire();
    }
}

bool AFPSCharacter::Server_StopFire_Validate()
{
    return true;
}

void AFPSCharacter::Server_StopFire_Implementation()
{
    if (InventoryComponent->GetCurrentWeapon())
    {
        InventoryComponent->GetCurrentWeapon()->Client_StopFire();
    }
}

void AFPSCharacter::Reload()
{
    if (IsNetMode(NM_DedicatedServer) || IsNetMode(NM_ListenServer))
    {
        if (InventoryComponent->GetCurrentWeapon())
        {
            InventoryComponent->GetCurrentWeapon()->Reload();
        }
    }
    else
    {
        Server_Reload();
    }
}

bool AFPSCharacter::Server_Reload_Validate()
{
    return true;
}

void AFPSCharacter::Server_Reload_Implementation()
{
    if (InventoryComponent->GetCurrentWeapon())
    {
        InventoryComponent->GetCurrentWeapon()->Reload();
    }
}