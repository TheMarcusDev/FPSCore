// Copyright 2022 Ellie Kelemen. All Rights Reserved.

#include "Components/InventoryComponent.h"
#include "EnhancedInputComponent.h"
#include "FPSCharacter.h"
#include "FPSCharacterController.h"
#include "WeaponBase.h"
#include "WeaponPickup.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"

// Sets default values for this component's properties
UInventoryComponent::UInventoryComponent()
{
}

// Swapping weapons with the scroll wheel
void UInventoryComponent::ScrollWeapon_Implementation(const FInputActionValue &Value)
{
	int NewID;

	// Value[0] determines the axis of input for our scroll wheel
	// a positive value indicates scrolling towards you, while a negative one
	// represents scrolling away from you

	if (Value[0] < 0)
	{
		NewID = FMath::Clamp(CurrentWeaponSlot + 1, 0, NumberOfWeaponSlots - 1);

		// If we've reached the end of the weapons array, loop back around to index 0
		if (CurrentWeaponSlot == NumberOfWeaponSlots - 1)
		{
			NewID = 0;
		}
	}
	else
	{
		NewID = FMath::Clamp(CurrentWeaponSlot - 1, 0, NumberOfWeaponSlots - 1);

		// If we've reached index 0, loop back around to the max index
		if (CurrentWeaponSlot == 0)
		{
			NewID = NumberOfWeaponSlots - 1;
		}
	}

	if (bPerformingWeaponSwap && WeaponSwapBehaviour == EWeaponSwapBehaviour::UseNewValue)
	{
		TargetWeaponSlot = NewID;
	}
	else if (!bPerformingWeaponSwap)
	{
		if (IsNetMode(NM_DedicatedServer) || IsNetMode(NM_ListenServer))
		{
			SwapWeapon(NewID);
		}
		else
		{
			Server_SwapWeapon(NewID);
		}
	}
}

void UInventoryComponent::BeginPlay()
{
	Super::BeginPlay();

	// Spawning starter weapons

	StarterWeapon();
}

void UInventoryComponent::StarterWeapon()
{
	for (int i = 0; i < NumberOfWeaponSlots; ++i)
	{
		if (StarterWeapons.IsValidIndex(i))
		{
			if (StarterWeapons[i].WeaponClassRef != nullptr)
			{
				// Getting a reference to our Weapon Data table in order to see if we have attachments
				const AWeaponBase *WeaponBaseReference = StarterWeapons[i].WeaponClassRef.GetDefaultObject();
				if (StarterWeapons[i].WeaponDataTableRef && WeaponBaseReference)
				{
					if (const FStaticWeaponData *WeaponData = StarterWeapons[i].WeaponDataTableRef->FindRow<FStaticWeaponData>(
							FName(WeaponBaseReference->GetDataTableNameRef()), FString(WeaponBaseReference->GetDataTableNameRef()),
							true))
					{
						// Spawning attachments if the weapon has them and the attachments table exists
						if (WeaponData->bHasAttachments && StarterWeapons[i].AttachmentsDataTable)
						{
							// Iterating through all the attachments in AttachmentArray
							for (FName RowName : StarterWeapons[i].DataStruct.WeaponAttachments)
							{
								// Searching the data table for the attachment
								const FAttachmentData *AttachmentData = StarterWeapons[i].AttachmentsDataTable->FindRow<FAttachmentData>(
									RowName, RowName.ToString(), true);

								// Applying the effects of the attachment
								if (AttachmentData)
								{
									if (AttachmentData->AttachmentType == EAttachmentType::Magazine)
									{
										// Pulling default values from the Magazine attachment type
										StarterWeapons[i].DataStruct.AmmoType = AttachmentData->AmmoToUse;
										StarterWeapons[i].DataStruct.ClipCapacity = AttachmentData->ClipCapacity;
										StarterWeapons[i].DataStruct.ClipSize = AttachmentData->ClipSize;
										StarterWeapons[i].DataStruct.WeaponHealth = 100.0f;
									}
								}
							}
						}
						else
						{
							StarterWeapons[i].DataStruct.AmmoType = WeaponData->AmmoToUse;
							StarterWeapons[i].DataStruct.ClipCapacity = WeaponData->ClipCapacity;
							StarterWeapons[i].DataStruct.ClipSize = WeaponData->ClipSize;
							StarterWeapons[i].DataStruct.WeaponHealth = 100.0f;
						}
					}
				}
				SpawnWeapon(StarterWeapons[i].WeaponClassRef, i, false, false, GetOwner()->GetActorTransform(), StarterWeapons[i].DataStruct);
			}
		}
	}
}

void UInventoryComponent::Server_SwapWeapon_Implementation(const int SlotId)
{
	SwapWeapon(SlotId);
}

void UInventoryComponent::SwapWeapon(const int SlotId)
{
	// Returning if the target weapon is already equipped or it does not exist
	if (CurrentWeaponSlot == SlotId)
	{
		return;
	}
	if (!EquippedWeapons.Contains(SlotId))
	{
		return;
	}
	if (!bPerformingWeaponSwap)
	{
		if (CurrentWeapon->GetStaticWeaponData()->WeaponUnequip)
		{
			CurrentWeapon->Client_StopFire();
			CurrentWeapon->SetCanFire(false);
			bPerformingWeaponSwap = true;
			TargetWeaponSlot = SlotId;
			CurrentWeapon->HandleUnequip(this);
			return;
		}
	}
	CurrentWeaponSlot = SlotId;

	// Disabling the currently equipped weapon, if it exists
	if (CurrentWeapon)
	{
		CurrentWeapon->PrimaryActorTick.bCanEverTick = false;
		CurrentWeapon->SetActorHiddenInGame(true);
		CurrentWeapon->SetCanFire(true);
		CurrentWeapon->Client_StopFire();
	}

	// Swapping to the new weapon, enabling it and playing it's equip animation
	CurrentWeapon = EquippedWeapons[SlotId];
	if (CurrentWeapon)
	{
		CurrentWeapon->PrimaryActorTick.bCanEverTick = true;
		CurrentWeapon->SetActorHiddenInGame(false);
		if (CurrentWeapon->GetStaticWeaponData()->WeaponEquip)
		{
			if (AFPSCharacter *FPSCharacter = Cast<AFPSCharacter>(GetOwner()))
			{
				FPSCharacter->UpdateMovementState(FPSCharacter->GetMovementState());
				CurrentWeapon->Multi_SwapWeaponAnim();
			}
		}
	}
	bPerformingWeaponSwap = false;
}

void UInventoryComponent::SpawnWeapon(TSubclassOf<AWeaponBase> NewWeapon, const int InventoryPosition, const bool bSpawnPickup,
									  const bool bStatic, const FTransform PickupTransform, const FRuntimeWeaponData DataStruct)
{
	// Determining spawn parameters (forcing the weapon pickup to spawn at all times)
	AFPSCharacter *CurrentPlayer = Cast<AFPSCharacter>(GetOwner());
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = CurrentPlayer;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (CurrentPlayer)
	{
		if (InventoryPosition == CurrentWeaponSlot && EquippedWeapons.Contains(InventoryPosition))
		{
			if (bSpawnPickup)
			{
				// Calculating the location where to spawn the new weapon in front of the player
				FVector TraceStart = FVector::ZeroVector;
				FRotator TraceStartRotation = FRotator::ZeroRotator;

				TraceStart = CurrentPlayer->GetCameraComponent()->GetComponentLocation();
				TraceStartRotation = CurrentPlayer->GetCameraComponent()->GetComponentRotation();

				const FVector TraceDirection = TraceStartRotation.Vector();
				const FVector TraceEnd = TraceStart + TraceDirection * WeaponSpawnDistance;

				// Spawning the new Weapon
				AWeaponPickup *NewPickup = GetWorld()->SpawnActor<AWeaponPickup>(CurrentWeapon->GetStaticWeaponData()->PickupReference, TraceEnd, FRotator::ZeroRotator, SpawnParameters);

				if (bStatic)
				{
					NewPickup->GetMainMesh()->SetSimulatePhysics(false);
					NewPickup->SetActorTransform(PickupTransform);
				}
				// Applying the current weapon data to the pickup
				NewPickup->SetStatic(bStatic);
				NewPickup->SetRuntimeSpawned(true);
				NewPickup->SetWeaponReference(EquippedWeapons[InventoryPosition]->GetClass());
				NewPickup->SetCacheDataStruct(EquippedWeapons[InventoryPosition]->GetRuntimeWeaponData());
				NewPickup->SpawnAttachmentMesh();
				EquippedWeapons[InventoryPosition]->Destroy();
			}
		}

		if (IsNetMode(NM_DedicatedServer) || IsNetMode(NM_ListenServer))
		{
			// Spawns the new weapon
			AWeaponBase *SpawnedWeapon = GetWorld()->SpawnActorDeferred<AWeaponBase>(NewWeapon, FTransform::Identity, CurrentPlayer, CurrentPlayer, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

			if (SpawnedWeapon)
			{
				// Placing the new weapon at the correct location and finishing up it's initialization

				SpawnedWeapon->SetOwner(CurrentPlayer);

				SpawnedWeapon->SetRuntimeWeaponData(DataStruct);
				SpawnedWeapon->MeshComp->CastShadow = true;
				SpawnedWeapon->SpawnAttachments();

				SpawnedWeapon->SetOwner(CurrentPlayer);
				SpawnedWeapon->FinishSpawning(FTransform::Identity);

				// Calling update weapon
				EquippedWeapons.Add(InventoryPosition, SpawnedWeapon);
				UpdateWeapon(SpawnedWeapon, InventoryPosition);
			}
		}
	}
}

// Spawns a new weapon (either from weapon swap, picking up a new weapon or starter weapon)
void UInventoryComponent::UpdateWeapon(AWeaponBase *SpawnedWeapon, const int InventoryPosition)
{
	AFPSCharacter *CurrentPlayer = Cast<AFPSCharacter>(GetOwner());
	if (CurrentPlayer == this->GetOwner())
	{
		// Disabling the currently equipped weapon, if it exists
		if (CurrentWeapon)
		{
			CurrentWeapon->PrimaryActorTick.bCanEverTick = false;
			CurrentWeapon->SetActorHiddenInGame(true);
			CurrentWeapon->Client_StopFire();
		}

		// Swapping to the new weapon, enabling it and playing it's equip animation
		CurrentWeapon = EquippedWeapons[InventoryPosition];
		CurrentWeaponSlot = InventoryPosition;

		if (CurrentWeapon)
		{
			CurrentWeapon->PrimaryActorTick.bCanEverTick = true;
			CurrentWeapon->SetActorHiddenInGame(false);

			if (CurrentWeapon->GetStaticWeaponData()->WeaponEquip)
			{
				if (CurrentPlayer)
				{
					CurrentPlayer->GetHandsMesh()->GetAnimInstance()->StopAllMontages(0.1f);
					CurrentPlayer->GetHandsMesh()->GetAnimInstance()->Montage_Play(CurrentWeapon->GetStaticWeaponData()->WeaponEquip, 1.0f);
					CurrentPlayer->UpdateMovementState(CurrentPlayer->GetMovementState());
				}
			}
		}
	}
}

FText UInventoryComponent::GetCurrentWeaponRemainingAmmo() const
{
	if (const AFPSCharacter *FPSCharacter = Cast<AFPSCharacter>(GetOwner()))
	{
		AFPSCharacterController *CharacterController = Cast<AFPSCharacterController>(FPSCharacter->GetController());
		if (CharacterController)
		{
			if (CurrentWeapon != nullptr)
			{
				return FText::AsNumber(CharacterController->AmmoMap[CurrentWeapon->GetRuntimeWeaponData()->AmmoType]);
			}
			UE_LOG(LogProfilingDebugging, Log, TEXT("Cannot find Current Weapon"));
			return FText::AsNumber(0);
		}
		UE_LOG(LogProfilingDebugging, Error, TEXT("No character controller found in UInventoryComponent"))
		return FText::FromString("Err");
	}
	UE_LOG(LogProfilingDebugging, Error, TEXT("No player character found in UInventoryComponent"))
	return FText::FromString("Err");
}

void UInventoryComponent::Inspect()
{
	if (CurrentWeapon)
	{
		if (const AFPSCharacter *FPSCharacter = Cast<AFPSCharacter>(GetOwner()))
		{
			if (CurrentWeapon->GetStaticWeaponData()->HandsInspect)
			{
				FPSCharacter->GetHandsMesh()->GetAnimInstance()->Montage_Play(CurrentWeapon->GetStaticWeaponData()->HandsInspect, 1.0f);
			}
			if (CurrentWeapon->GetStaticWeaponData()->WeaponInspect)
			{
				CurrentWeapon->GetMainMeshComp()->PlayAnimation(CurrentWeapon->GetStaticWeaponData()->WeaponInspect, false);
			}
		}
	}
}

void UInventoryComponent::UnequipReturn()
{
	SwapWeapon(TargetWeaponSlot);
}

void UInventoryComponent::SetupInputComponent(UEnhancedInputComponent *PlayerInputComponent)
{
	if (PrimaryWeaponAction)
	{
		// Switching to the primary weapon
		if (IsNetMode(NM_DedicatedServer) || IsNetMode(NM_ListenServer))
		{
			PlayerInputComponent->BindAction(PrimaryWeaponAction, ETriggerEvent::Started, this, &UInventoryComponent::SwapWeapon<0>);
		}
		else
		{
			PlayerInputComponent->BindAction(PrimaryWeaponAction, ETriggerEvent::Started, this, &UInventoryComponent::Server_SwapWeapon<0>);
		}
	}

	if (SecondaryWeaponAction)
	{
		// Switching to the secondary weapon
		if (IsNetMode(NM_DedicatedServer) || IsNetMode(NM_ListenServer))
		{
			PlayerInputComponent->BindAction(SecondaryWeaponAction, ETriggerEvent::Started, this, &UInventoryComponent::SwapWeapon<1>);
		}
		else
		{
			PlayerInputComponent->BindAction(SecondaryWeaponAction, ETriggerEvent::Started, this, &UInventoryComponent::Server_SwapWeapon<1>);
		}
	}

	if (ScrollAction)
	{
		// Scrolling through weapons
		PlayerInputComponent->BindAction(ScrollAction, ETriggerEvent::Started, this, &UInventoryComponent::ScrollWeapon);
	}

	if (InspectWeaponAction)
	{
		// Playing the inspect animation
		PlayerInputComponent->BindAction(InspectWeaponAction, ETriggerEvent::Started, this, &UInventoryComponent::Inspect);
	}
}
