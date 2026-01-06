// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/UtilityService.h"
#include "GameThreadDispatcher.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Editor/Transactor.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Editor/UnrealEdTypes.h"
#include "Selection.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "SceneView.h"

FUtilityService::FUtilityService()
{
}

FString FUtilityService::GetServiceDescription() const
{
	return TEXT("Editor utilities - save, undo/redo, and selection management");
}

FMCPResponse FUtilityService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("save_level")) return HandleSaveLevel(Request);
	if (MethodName == TEXT("undo")) return HandleUndo(Request);
	if (MethodName == TEXT("redo")) return HandleRedo(Request);
	if (MethodName == TEXT("select_actor")) return HandleSelectActor(Request);
	if (MethodName == TEXT("get_selection")) return HandleGetSelection(Request);
	if (MethodName == TEXT("get_selection_bounds")) return HandleGetSelectionBounds(Request);
	if (MethodName == TEXT("select_at_screen")) return HandleSelectAtScreen(Request);

	return MethodNotFound(Request.Id, TEXT("utility"), MethodName);
}

FMCPResponse FUtilityService::HandleSaveLevel(const FMCPRequest& Request)
{
	auto SaveTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Save current level
		bool bSaved = FEditorFileUtils::SaveCurrentLevel();

		Result->SetBoolField(TEXT("success"), bSaved);
		if (bSaved)
		{
			Result->SetStringField(TEXT("message"), TEXT("Level saved successfully"));
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Level saved"));
		}
		else
		{
			Result->SetStringField(TEXT("error"), TEXT("Failed to save level"));
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SaveTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleUndo(const FMCPRequest& Request)
{
	int32 Steps = 1;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("steps"), Steps);
	}

	auto UndoTask = [Steps]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		for (int32 i = 0; i < Steps; i++)
		{
			if (GEditor->Trans->CanUndo())
			{
				GEditor->Trans->Undo();
			}
			else
			{
				break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("steps_undone"), Steps);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Undo %d steps"), Steps);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(UndoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleRedo(const FMCPRequest& Request)
{
	int32 Steps = 1;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("steps"), Steps);
	}

	auto RedoTask = [Steps]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		for (int32 i = 0; i < Steps; i++)
		{
			if (GEditor->Trans->CanRedo())
			{
				GEditor->Trans->Redo();
			}
			else
			{
				break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("steps_redone"), Steps);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Redo %d steps"), Steps);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(RedoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleSelectActor(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString ActorName;
	if (!Request.Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'actor_name'"));
	}

	bool bAddToSelection = false;
	Request.Params->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);

	auto SelectTask = [ActorName, bAddToSelection]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Find actor by label
		AActor* FoundActor = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == ActorName)
			{
				FoundActor = *It;
				break;
			}
		}

		if (!FoundActor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			return Result;
		}

		// Select the actor
		if (!bAddToSelection)
		{
			GEditor->SelectNone(true, true);
		}
		GEditor->SelectActor(FoundActor, true, true);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("actor_name"), ActorName);
		Result->SetBoolField(TEXT("added_to_selection"), bAddToSelection);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Selected actor: %s"), *ActorName);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SelectTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleGetSelection(const FMCPRequest& Request)
{
	auto GetSelTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> SelectedActors;
		
		// Get selected actors using the editor selection set
		USelection* SelectedActorsObj = GEditor->GetSelectedActors();
		if (SelectedActorsObj)
		{
			TArray<AActor*> ActorArray;
			SelectedActorsObj->GetSelectedObjects<AActor>(ActorArray);
			
			for (AActor* Actor : ActorArray)
			{
				if (Actor)
				{
					TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
					ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
					ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					
					SelectedActors.Add(MakeShared<FJsonValueObject>(ActorObj));
				}
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("selected_actors"), SelectedActors);
		Result->SetNumberField(TEXT("count"), SelectedActors.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetSelTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleGetSelectionBounds(const FMCPRequest& Request)
{
	auto GetBoundsTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (!GEditor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("GEditor not available"));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ActorBoundsArray;
		
		USelection* SelectedActorsObj = GEditor->GetSelectedActors();
		if (SelectedActorsObj)
		{
			TArray<AActor*> ActorArray;
			SelectedActorsObj->GetSelectedObjects<AActor>(ActorArray);
			
			for (AActor* Actor : ActorArray)
			{
				if (!Actor) continue;
				
				TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
				ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
				ActorObj->SetStringField(TEXT("id"), Actor->GetName());
				ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				
				// Get actor location
				FVector Location = Actor->GetActorLocation();
				TArray<TSharedPtr<FJsonValue>> LocArr;
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.X));
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.Y));
				LocArr.Add(MakeShared<FJsonValueNumber>(Location.Z));
				ActorObj->SetArrayField(TEXT("location"), LocArr);
				
				// Get actor rotation
				FRotator Rotation = Actor->GetActorRotation();
				TArray<TSharedPtr<FJsonValue>> RotArr;
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
				RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
				ActorObj->SetArrayField(TEXT("rotation"), RotArr);
				
				// Get actor scale
				FVector Scale = Actor->GetActorScale3D();
				TArray<TSharedPtr<FJsonValue>> ScaleArr;
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.X));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Y));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(Scale.Z));
				ActorObj->SetArrayField(TEXT("scale"), ScaleArr);
				
				// Get forward vector (direction actor is facing)
				FVector ForwardVector = Actor->GetActorForwardVector();
				TArray<TSharedPtr<FJsonValue>> ForwardArr;
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.X));
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.Y));
				ForwardArr.Add(MakeShared<FJsonValueNumber>(ForwardVector.Z));
				ActorObj->SetArrayField(TEXT("forward_vector"), ForwardArr);
				
				// Get right vector
				FVector RightVector = Actor->GetActorRightVector();
				TArray<TSharedPtr<FJsonValue>> RightArr;
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.X));
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.Y));
				RightArr.Add(MakeShared<FJsonValueNumber>(RightVector.Z));
				ActorObj->SetArrayField(TEXT("right_vector"), RightArr);
				
				// Get up vector
				FVector UpVector = Actor->GetActorUpVector();
				TArray<TSharedPtr<FJsonValue>> UpArr;
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.X));
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.Y));
				UpArr.Add(MakeShared<FJsonValueNumber>(UpVector.Z));
				ActorObj->SetArrayField(TEXT("up_vector"), UpArr);
				
				// Get bounding box (all components combined)
				FBox BoundingBox = Actor->GetComponentsBoundingBox();
				if (BoundingBox.IsValid)
				{
					TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
					
					// Min point
					TArray<TSharedPtr<FJsonValue>> MinArr;
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
					MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
					BoundsObj->SetArrayField(TEXT("min"), MinArr);
					
					// Max point
					TArray<TSharedPtr<FJsonValue>> MaxArr;
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
					MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
					BoundsObj->SetArrayField(TEXT("max"), MaxArr);
					
					// Center point
					FVector Center = BoundingBox.GetCenter();
					TArray<TSharedPtr<FJsonValue>> CenterArr;
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
					BoundsObj->SetArrayField(TEXT("center"), CenterArr);
					
					// Extent (half-size)
					FVector Extent = BoundingBox.GetExtent();
					TArray<TSharedPtr<FJsonValue>> ExtentArr;
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.X));
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Y));
					ExtentArr.Add(MakeShared<FJsonValueNumber>(Extent.Z));
					BoundsObj->SetArrayField(TEXT("extent"), ExtentArr);
					
					// Size (full dimensions)
					FVector Size = BoundingBox.GetSize();
					TArray<TSharedPtr<FJsonValue>> SizeArr;
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
					SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
					BoundsObj->SetArrayField(TEXT("size"), SizeArr);
					
					ActorObj->SetObjectField(TEXT("bounds"), BoundsObj);
				}
				
				ActorBoundsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("actors"), ActorBoundsArray);
		Result->SetNumberField(TEXT("count"), ActorBoundsArray.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for %d selected actors"), ActorBoundsArray.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetBoundsTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FUtilityService::HandleSelectAtScreen(const FMCPRequest& Request)
{
	// Get screen position as percentage (0.0 to 1.0)
	double ScreenX = 0.5;  // Default to center
	double ScreenY = 0.5;
	bool bAddToSelection = false;
	
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("screen_x"), ScreenX);
		Request.Params->TryGetNumberField(TEXT("screen_y"), ScreenY);
		Request.Params->TryGetBoolField(TEXT("add_to_selection"), bAddToSelection);
	}
	
	// Clamp to valid range
	ScreenX = FMath::Clamp(ScreenX, 0.0, 1.0);
	ScreenY = FMath::Clamp(ScreenY, 0.0, 1.0);

	auto SelectTask = [ScreenX, ScreenY, bAddToSelection]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport found"));
			return Result;
		}

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(Viewport->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Get viewport size and calculate pixel position
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		int32 PixelX = FMath::RoundToInt(ScreenX * ViewportSize.X);
		int32 PixelY = FMath::RoundToInt(ScreenY * ViewportSize.Y);

		// Get ray from screen position
		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			Viewport,
			ViewportClient->GetScene(),
			ViewportClient->EngineShowFlags)
			.SetRealtimeUpdate(true));
		
		FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to calculate scene view"));
			return Result;
		}

		// Deproject screen position to world ray
		FVector WorldOrigin, WorldDirection;
		SceneView->DeprojectFVector2D(FVector2D(PixelX, PixelY), WorldOrigin, WorldDirection);

		// Perform line trace
		FHitResult HitResult;
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(SelectAtScreen), true);
		
		float TraceDistance = 100000.0f; // 1km trace distance
		FVector TraceEnd = WorldOrigin + WorldDirection * TraceDistance;
		
		bool bHit = World->LineTraceSingleByChannel(
			HitResult,
			WorldOrigin,
			TraceEnd,
			ECC_Visibility,
			TraceParams
		);

		// Store input parameters
		Result->SetNumberField(TEXT("screen_x"), ScreenX);
		Result->SetNumberField(TEXT("screen_y"), ScreenY);

		if (bHit && HitResult.GetActor())
		{
			AActor* HitActor = HitResult.GetActor();
			
			// Select the actor
			if (!bAddToSelection)
			{
				GEditor->SelectNone(true, true);
			}
			GEditor->SelectActor(HitActor, true, true);
			
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), true);
			Result->SetStringField(TEXT("actor_name"), HitActor->GetActorLabel());
			Result->SetStringField(TEXT("actor_id"), HitActor->GetName());
			Result->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetName());
			
			// Hit location
			TArray<TSharedPtr<FJsonValue>> LocationArr;
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.X));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Y));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Z));
			Result->SetArrayField(TEXT("hit_location"), LocationArr);
			
			// Actor location
			FVector ActorLoc = HitActor->GetActorLocation();
			TArray<TSharedPtr<FJsonValue>> ActorLocArr;
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.X));
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.Y));
			ActorLocArr.Add(MakeShared<FJsonValueNumber>(ActorLoc.Z));
			Result->SetArrayField(TEXT("actor_location"), ActorLocArr);
			
			// Actor rotation
			FRotator ActorRot = HitActor->GetActorRotation();
			TArray<TSharedPtr<FJsonValue>> ActorRotArr;
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Pitch));
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Yaw));
			ActorRotArr.Add(MakeShared<FJsonValueNumber>(ActorRot.Roll));
			Result->SetArrayField(TEXT("actor_rotation"), ActorRotArr);
			
			// Actor scale
			FVector ActorScale = HitActor->GetActorScale3D();
			TArray<TSharedPtr<FJsonValue>> ActorScaleArr;
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.X));
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.Y));
			ActorScaleArr.Add(MakeShared<FJsonValueNumber>(ActorScale.Z));
			Result->SetArrayField(TEXT("actor_scale"), ActorScaleArr);
			
			// Bounding box
			FBox BoundingBox = HitActor->GetComponentsBoundingBox();
			if (BoundingBox.IsValid)
			{
				TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
				
				TArray<TSharedPtr<FJsonValue>> MinArr;
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
				MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
				BoundsObj->SetArrayField(TEXT("min"), MinArr);
				
				TArray<TSharedPtr<FJsonValue>> MaxArr;
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.X));
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Y));
				MaxArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Max.Z));
				BoundsObj->SetArrayField(TEXT("max"), MaxArr);
				
				FVector Size = BoundingBox.GetSize();
				TArray<TSharedPtr<FJsonValue>> SizeArr;
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
				SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
				BoundsObj->SetArrayField(TEXT("size"), SizeArr);
				
				Result->SetObjectField(TEXT("bounds"), BoundsObj);
			}
			
			// Tags
			TArray<TSharedPtr<FJsonValue>> TagsArr;
			for (const FName& Tag : HitActor->Tags)
			{
				TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			Result->SetArrayField(TEXT("tags"), TagsArr);

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Selected actor at screen (%.2f, %.2f): %s"), 
				ScreenX, ScreenY, *HitActor->GetActorLabel());
		}
		else
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), false);
			Result->SetStringField(TEXT("message"), TEXT("No actor at screen position"));
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: No actor at screen (%.2f, %.2f)"), ScreenX, ScreenY);
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SelectTask);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FUtilityService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// save_level
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("save_level");
		Tool.Description = TEXT("Save the current level to disk.");
		Tools.Add(Tool);
	}
	
	// undo
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("undo");
		Tool.Description = TEXT("Undo the last editor action.");
		
		TSharedPtr<FJsonObject> StepsParam = MakeShared<FJsonObject>();
		StepsParam->SetStringField(TEXT("type"), TEXT("number"));
		StepsParam->SetStringField(TEXT("description"), TEXT("Number of undo steps (default: 1)"));
		Tool.Parameters->SetObjectField(TEXT("steps"), StepsParam);
		
		Tools.Add(Tool);
	}
	
	// redo
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("redo");
		Tool.Description = TEXT("Redo a previously undone action.");
		
		TSharedPtr<FJsonObject> StepsParam = MakeShared<FJsonObject>();
		StepsParam->SetStringField(TEXT("type"), TEXT("number"));
		StepsParam->SetStringField(TEXT("description"), TEXT("Number of redo steps (default: 1)"));
		Tool.Parameters->SetObjectField(TEXT("steps"), StepsParam);
		
		Tools.Add(Tool);
	}
	
	// select_actor
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("select_actor");
		Tool.Description = TEXT("Select an actor in the editor.");
		
		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("The actor name to select"));
		Tool.Parameters->SetObjectField(TEXT("actor_name"), NameParam);
		Tool.RequiredParams.Add(TEXT("actor_name"));
		
		TSharedPtr<FJsonObject> AddParam = MakeShared<FJsonObject>();
		AddParam->SetStringField(TEXT("type"), TEXT("boolean"));
		AddParam->SetStringField(TEXT("description"), TEXT("Add to current selection instead of replacing (default: false)"));
		Tool.Parameters->SetObjectField(TEXT("add_to_selection"), AddParam);
		
		Tools.Add(Tool);
	}
	
	// get_selection
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_selection");
		Tool.Description = TEXT("Get the currently selected actors in the editor.");
		Tools.Add(Tool);
	}
	
	// get_selection_bounds
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_selection_bounds");
		Tool.Description = TEXT("Get detailed bounds and orientation data for selected actors. Returns location, rotation, scale, forward/right/up vectors, and bounding box (min, max, center, extent, size) for each selected actor.");
		Tools.Add(Tool);
	}
	
	// select_at_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("select_at_screen");
		Tool.Description = TEXT("Select an actor by clicking a point in the screenshot. Workflow: screenshot -> see actor -> estimate % position -> select. Returns FULL actor info: name, class, location, rotation, scale, bounds, tags. Use to identify unknown actors or get their exact transforms.");
		
		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Screen X as 0-1 percentage (0=left edge, 0.5=center, 1=right edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_x"), XParam);
		
		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Screen Y as 0-1 percentage (0=top edge, 0.5=center, 1=bottom edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_y"), YParam);
		
		TSharedPtr<FJsonObject> AddParam = MakeShared<FJsonObject>();
		AddParam->SetStringField(TEXT("type"), TEXT("boolean"));
		AddParam->SetStringField(TEXT("description"), TEXT("Add to current selection instead of replacing (default: false)"));
		Tool.Parameters->SetObjectField(TEXT("add_to_selection"), AddParam);
		
		Tools.Add(Tool);
	}
	
	return Tools;
}
