// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ViewportService.h"
#include "GameThreadDispatcher.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "SceneView.h"

FViewportService::FViewportService()
{
}

FString FViewportService::GetServiceDescription() const
{
	return TEXT("Viewport camera control - position camera for screenshot capture");
}

FMCPResponse FViewportService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("set_location")) return HandleSetLocation(Request);
	if (MethodName == TEXT("set_rotation")) return HandleSetRotation(Request);
	if (MethodName == TEXT("get_transform")) return HandleGetTransform(Request);
	if (MethodName == TEXT("focus_actor")) return HandleFocusActor(Request);
	if (MethodName == TEXT("trace_from_screen")) return HandleTraceFromScreen(Request);

	return MethodNotFound(Request.Id, TEXT("viewport"), MethodName);
}

FMCPResponse FViewportService::HandleSetLocation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get location array
	const TArray<TSharedPtr<FJsonValue>>* LocationArray;
	if (!Request.Params->TryGetArrayField(TEXT("location"), LocationArray) || LocationArray->Num() != 3)
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'location' parameter (expected array of 3 numbers)"));
	}

	FVector Location(
		(*LocationArray)[0]->AsNumber(),
		(*LocationArray)[1]->AsNumber(),
		(*LocationArray)[2]->AsNumber()
	);

	auto SetLocationTask = [Location]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		ViewportClient->SetViewLocation(Location);
		
		Result->SetBoolField(TEXT("success"), true);
		TArray<TSharedPtr<FJsonValue>> LocationJson;
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Z));
		Result->SetArrayField(TEXT("location"), LocationJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Viewport location set to: (%.1f, %.1f, %.1f)"), 
			Location.X, Location.Y, Location.Z);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SetLocationTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleSetRotation(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get rotation array
	const TArray<TSharedPtr<FJsonValue>>* RotationArray;
	if (!Request.Params->TryGetArrayField(TEXT("rotation"), RotationArray) || RotationArray->Num() != 3)
	{
		return InvalidParams(Request.Id, TEXT("Missing or invalid 'rotation' parameter (expected array of 3 numbers)"));
	}

	FRotator Rotation(
		(*RotationArray)[0]->AsNumber(),
		(*RotationArray)[1]->AsNumber(),
		(*RotationArray)[2]->AsNumber()
	);

	auto SetRotationTask = [Rotation]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		ViewportClient->SetViewRotation(Rotation);
		
		Result->SetBoolField(TEXT("success"), true);
		TArray<TSharedPtr<FJsonValue>> RotationJson;
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
		Result->SetArrayField(TEXT("rotation"), RotationJson);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Viewport rotation set to: (%.1f, %.1f, %.1f)"), 
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SetRotationTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleGetTransform(const FMCPRequest& Request)
{
	auto GetTransformTask = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport client found"));
			return Result;
		}

		FVector Location = ViewportClient->GetViewLocation();
		FRotator Rotation = ViewportClient->GetViewRotation();

		TArray<TSharedPtr<FJsonValue>> LocationJson;
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationJson.Add(MakeShared<FJsonValueNumber>(Location.Z));

		TArray<TSharedPtr<FJsonValue>> RotationJson;
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationJson.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("location"), LocationJson);
		Result->SetArrayField(TEXT("rotation"), RotationJson);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetTransformTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleFocusActor(const FMCPRequest& Request)
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

	auto FocusTask = [ActorName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No editor world found"));
			return Result;
		}

		// Find actor by label first, then by internal name (ID)
		AActor* FoundActor = nullptr;
		FString MatchedBy;
		
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			
			// First priority: exact label match
			if (Actor->GetActorLabel() == ActorName)
			{
				FoundActor = Actor;
				MatchedBy = TEXT("label");
				break;
			}
			// Second priority: exact internal name match (like pressing F in editor)
			if (Actor->GetName() == ActorName)
			{
				FoundActor = Actor;
				MatchedBy = TEXT("name");
				break;
			}
		}
		
		// If no exact match, try case-insensitive partial match on label
		if (!FoundActor)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;
				
				if (Actor->GetActorLabel().Contains(ActorName, ESearchCase::IgnoreCase) ||
				    Actor->GetName().Contains(ActorName, ESearchCase::IgnoreCase))
				{
					FoundActor = Actor;
					MatchedBy = TEXT("partial");
					break;
				}
			}
		}

		if (!FoundActor)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			return Result;
		}

		// Focus on the actor (like pressing F in editor)
		FLevelEditorViewportClient* ViewportClient = static_cast<FLevelEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
		if (ViewportClient)
		{
			ViewportClient->FocusViewportOnBox(FoundActor->GetComponentsBoundingBox());
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("actor_name"), FoundActor->GetActorLabel());
		Result->SetStringField(TEXT("actor_id"), FoundActor->GetName());
		Result->SetStringField(TEXT("matched_by"), MatchedBy);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Focused viewport on actor: %s (ID: %s, matched by: %s)"), 
			*FoundActor->GetActorLabel(), *FoundActor->GetName(), *MatchedBy);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(FocusTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FViewportService::HandleTraceFromScreen(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	// Get screen position as percentage (0.0 to 1.0)
	double ScreenX = 0.5;  // Default to center
	double ScreenY = 0.5;
	
	Request.Params->TryGetNumberField(TEXT("screen_x"), ScreenX);
	Request.Params->TryGetNumberField(TEXT("screen_y"), ScreenY);
	
	// Clamp to valid range
	ScreenX = FMath::Clamp(ScreenX, 0.0, 1.0);
	ScreenY = FMath::Clamp(ScreenY, 0.0, 1.0);

	auto TraceTask = [ScreenX, ScreenY]() -> TSharedPtr<FJsonObject>
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
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(ScreenTrace), true);
		TraceParams.bReturnPhysicalMaterial = true;
		
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
		Result->SetNumberField(TEXT("pixel_x"), PixelX);
		Result->SetNumberField(TEXT("pixel_y"), PixelY);
		Result->SetNumberField(TEXT("viewport_width"), ViewportSize.X);
		Result->SetNumberField(TEXT("viewport_height"), ViewportSize.Y);

		if (bHit)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), true);
			
			// Hit location
			TArray<TSharedPtr<FJsonValue>> LocationArr;
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.X));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Y));
			LocationArr.Add(MakeShared<FJsonValueNumber>(HitResult.Location.Z));
			Result->SetArrayField(TEXT("location"), LocationArr);
			
			// Impact normal (surface normal at hit point)
			TArray<TSharedPtr<FJsonValue>> NormalArr;
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.X));
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.Y));
			NormalArr.Add(MakeShared<FJsonValueNumber>(HitResult.ImpactNormal.Z));
			Result->SetArrayField(TEXT("normal"), NormalArr);
			
			// Distance from camera
			Result->SetNumberField(TEXT("distance"), HitResult.Distance);
			
			// Hit actor info
			AActor* HitActor = HitResult.GetActor();
			if (HitActor)
			{
				Result->SetStringField(TEXT("actor_name"), HitActor->GetActorLabel());
				Result->SetStringField(TEXT("actor_id"), HitActor->GetName());
				Result->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetName());
			}
			
			// Hit component info
			UPrimitiveComponent* HitComponent = HitResult.GetComponent();
			if (HitComponent)
			{
				Result->SetStringField(TEXT("component_name"), HitComponent->GetName());
			}
			
			// Physical material if available
			if (HitResult.PhysMaterial.IsValid())
			{
				Result->SetStringField(TEXT("physical_material"), HitResult.PhysMaterial->GetName());
			}

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screen trace hit at (%.1f, %.1f) -> Location: (%.1f, %.1f, %.1f), Normal: (%.2f, %.2f, %.2f)"),
				ScreenX, ScreenY, HitResult.Location.X, HitResult.Location.Y, HitResult.Location.Z,
				HitResult.ImpactNormal.X, HitResult.ImpactNormal.Y, HitResult.ImpactNormal.Z);
		}
		else
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("hit"), false);
			Result->SetStringField(TEXT("message"), TEXT("No hit - ray did not intersect any geometry"));
			
			// Still return the ray direction for reference
			TArray<TSharedPtr<FJsonValue>> DirArr;
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.X));
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.Y));
			DirArr.Add(MakeShared<FJsonValueNumber>(WorldDirection.Z));
			Result->SetArrayField(TEXT("ray_direction"), DirArr);

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screen trace at (%.1f, %.1f) - no hit"), ScreenX, ScreenY);
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(TraceTask);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FViewportService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// set_location
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_location");
		Tool.Description = TEXT("Set the viewport camera location.");
		
		TSharedPtr<FJsonObject> LocParam = MakeShared<FJsonObject>();
		LocParam->SetStringField(TEXT("type"), TEXT("array"));
		LocParam->SetStringField(TEXT("description"), TEXT("Camera location as [X, Y, Z]"));
		Tool.Parameters->SetObjectField(TEXT("location"), LocParam);
		Tool.RequiredParams.Add(TEXT("location"));
		
		Tools.Add(Tool);
	}
	
	// set_rotation
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_rotation");
		Tool.Description = TEXT("Set the viewport camera rotation.");
		
		TSharedPtr<FJsonObject> RotParam = MakeShared<FJsonObject>();
		RotParam->SetStringField(TEXT("type"), TEXT("array"));
		RotParam->SetStringField(TEXT("description"), TEXT("Camera rotation as [Pitch, Yaw, Roll]"));
		Tool.Parameters->SetObjectField(TEXT("rotation"), RotParam);
		Tool.RequiredParams.Add(TEXT("rotation"));
		
		Tools.Add(Tool);
	}
	
	// get_transform
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_transform");
		Tool.Description = TEXT("Get the current viewport camera location and rotation.");
		Tools.Add(Tool);
	}
	
	// focus_actor
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("focus_actor");
		Tool.Description = TEXT("Frame an actor in the viewport (like pressing F). Use to navigate to any actor by name. Get actor names from world/list_actors or utility/select_at_screen. After focusing, take a screenshot to see it.");
		
		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("The actor label or internal name/ID to focus on"));
		Tool.Parameters->SetObjectField(TEXT("actor_name"), NameParam);
		Tool.RequiredParams.Add(TEXT("actor_name"));
		
		Tools.Add(Tool);
	}
	
	// trace_from_screen
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("trace_from_screen");
		Tool.Description = TEXT("ESSENTIAL: Get 3D location AND surface normal from any point in screenshot. Use to: 1) Find WHERE to place actors (location), 2) Find HOW to orient actors (normal = surface 'up' direction). Workflow: screenshot -> see point -> trace at that % position -> get location+normal -> spawn/orient actor.");
		
		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Screen X as 0-1 percentage (0=left edge, 0.5=center, 1=right edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_x"), XParam);
		
		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Screen Y as 0-1 percentage (0=top edge, 0.5=center, 1=bottom edge). Estimate from screenshot."));
		Tool.Parameters->SetObjectField(TEXT("screen_y"), YParam);
		
		Tools.Add(Tool);
	}
	
	return Tools;
}
