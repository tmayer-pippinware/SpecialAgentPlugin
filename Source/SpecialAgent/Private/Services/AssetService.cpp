// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/AssetService.h"
#include "GameThreadDispatcher.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"

FAssetService::FAssetService()
{
}

FString FAssetService::GetServiceDescription() const
{
	return TEXT("Asset discovery and management - browse Content Browser assets");
}

FMCPResponse FAssetService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("list"))
	{
		return HandleListAssets(Request);
	}
	else if (MethodName == TEXT("find"))
	{
		return HandleFindAsset(Request);
	}
	else if (MethodName == TEXT("get_properties"))
	{
		return HandleGetAssetProperties(Request);
	}
	else if (MethodName == TEXT("search"))
	{
		return HandleSearchAssets(Request);
	}
	else if (MethodName == TEXT("get_bounds"))
	{
		return HandleGetAssetBounds(Request);
	}
	else if (MethodName == TEXT("get_info"))
	{
		return HandleGetAssetInfo(Request);
	}

	return MethodNotFound(Request.Id, TEXT("assets"), MethodName);
}

FMCPResponse FAssetService::HandleListAssets(const FMCPRequest& Request)
{
	// Get filter parameters
	FString ClassFilter;
	FString PathFilter;
	int32 MaxResults = 1000;

	if (Request.Params.IsValid())
	{
		const TSharedPtr<FJsonObject>* FilterObj;
		if (Request.Params->TryGetObjectField(TEXT("filter"), FilterObj))
		{
			(*FilterObj)->TryGetStringField(TEXT("class"), ClassFilter);
			(*FilterObj)->TryGetStringField(TEXT("path"), PathFilter);
			(*FilterObj)->TryGetNumberField(TEXT("max_results"), MaxResults);
		}
	}

	auto ListTask = [ClassFilter, PathFilter, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		// Build filter
		FARFilter Filter;
		if (!ClassFilter.IsEmpty())
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(ClassFilter));
		}
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.bRecursivePaths = true;
		}

		// Get assets
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssets(Filter, AssetDataList);

		// Limit results
		if (AssetDataList.Num() > MaxResults)
		{
			AssetDataList.SetNum(MaxResults);
		}

		// Convert to JSON array
		TArray<TSharedPtr<FJsonValue>> AssetsJson;
		for (const FAssetData& AssetData : AssetDataList)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
			AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
			AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
			AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			
			AssetsJson.Add(MakeShared<FJsonValueObject>(AssetObj));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), AssetsJson);
		Result->SetNumberField(TEXT("count"), AssetsJson.Num());
		Result->SetNumberField(TEXT("total_found"), AssetDataList.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Listed %d assets"), AssetsJson.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(ListTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleFindAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Name;
	if (!Request.Params->TryGetStringField(TEXT("name"), Name))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'name'"));
	}

	auto FindTask = [Name]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		// Search for assets matching the name
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAllAssets(AssetDataList);

		TArray<TSharedPtr<FJsonValue>> MatchingAssets;
		for (const FAssetData& AssetData : AssetDataList)
		{
			if (AssetData.AssetName.ToString().Contains(Name))
			{
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
				AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
				
				MatchingAssets.Add(MakeShared<FJsonValueObject>(AssetObj));

				if (MatchingAssets.Num() >= 100) break; // Limit results
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), MatchingAssets);
		Result->SetNumberField(TEXT("count"), MatchingAssets.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(FindTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetProperties(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetPropertiesTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		
		if (!AssetData.IsValid())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
			return Result;
		}

		// Build properties object
		TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
		PropertiesObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		PropertiesObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		PropertiesObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
		PropertiesObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		PropertiesObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());

		// Get tags
		TArray<TSharedPtr<FJsonValue>> TagsArray;
		for (const auto& TagPair : AssetData.TagsAndValues)
		{
			TSharedPtr<FJsonObject> TagObj = MakeShared<FJsonObject>();
			TagObj->SetStringField(TEXT("key"), TagPair.Key.ToString());
			TagObj->SetStringField(TEXT("value"), TagPair.Value.AsString());
			TagsArray.Add(MakeShared<FJsonValueObject>(TagObj));
		}
		PropertiesObj->SetArrayField(TEXT("tags"), TagsArray);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("properties"), PropertiesObj);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetPropertiesTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleSearchAssets(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString Query;
	if (!Request.Params->TryGetStringField(TEXT("query"), Query))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'query'"));
	}

	int32 MaxResults = 100;
	Request.Params->TryGetNumberField(TEXT("max_results"), MaxResults);

	auto SearchTask = [Query, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAllAssets(AssetDataList);

		// Search in name, path, and class
		FString QueryLower = Query.ToLower();
		TArray<TSharedPtr<FJsonValue>> MatchingAssets;
		
		for (const FAssetData& AssetData : AssetDataList)
		{
			bool bMatches = false;
			
			if (AssetData.AssetName.ToString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}
			else if (AssetData.GetObjectPathString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}
			else if (AssetData.AssetClassPath.ToString().ToLower().Contains(QueryLower))
			{
				bMatches = true;
			}

			if (bMatches)
			{
				TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
				AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
				AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
				
				MatchingAssets.Add(MakeShared<FJsonValueObject>(AssetObj));

				if (MatchingAssets.Num() >= MaxResults) break;
			}
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("assets"), MatchingAssets);
		Result->SetNumberField(TEXT("count"), MatchingAssets.Num());
		Result->SetStringField(TEXT("query"), Query);

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Search for '%s' found %d assets"), *Query, MatchingAssets.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SearchTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetBounds(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetBoundsTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Try to load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found or failed to load: %s"), *AssetPath));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

		// Check if it's a StaticMesh
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh)
		{
			FBox BoundingBox = StaticMesh->GetBoundingBox();
			
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
			
			// Pivot offset - how far the origin (0,0,0) is from the mesh center
			// Negative values mean the pivot is below/behind center
			// Use -Center to get offset needed to center the mesh at spawn point
			TArray<TSharedPtr<FJsonValue>> PivotOffsetArr;
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.X));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Y));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Z));
			BoundsObj->SetArrayField(TEXT("pivot_offset"), PivotOffsetArr);
			
			// Bottom offset - how far below origin the mesh extends (add to Z to place on ground)
			BoundsObj->SetNumberField(TEXT("bottom_z"), -BoundingBox.Min.Z);
			
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			Result->SetStringField(TEXT("mesh_type"), TEXT("StaticMesh"));
			
			// Additional mesh info
			Result->SetNumberField(TEXT("num_lods"), StaticMesh->GetNumLODs());
			if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
			{
				Result->SetNumberField(TEXT("num_vertices"), StaticMesh->GetRenderData()->LODResources[0].GetNumVertices());
				Result->SetNumberField(TEXT("num_triangles"), StaticMesh->GetRenderData()->LODResources[0].GetNumTriangles());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for StaticMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a SkeletalMesh
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
		if (SkeletalMesh)
		{
			FBox BoundingBox = SkeletalMesh->GetBounds().GetBox();
			
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
			
			// Pivot offset and bottom Z
			TArray<TSharedPtr<FJsonValue>> PivotOffsetArr;
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.X));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Y));
			PivotOffsetArr.Add(MakeShared<FJsonValueNumber>(-Center.Z));
			BoundsObj->SetArrayField(TEXT("pivot_offset"), PivotOffsetArr);
			BoundsObj->SetNumberField(TEXT("bottom_z"), -BoundingBox.Min.Z);
			
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			Result->SetStringField(TEXT("mesh_type"), TEXT("SkeletalMesh"));
			
			// Additional skeletal mesh info
			Result->SetNumberField(TEXT("num_bones"), SkeletalMesh->GetRefSkeleton().GetNum());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got bounds for SkeletalMesh: %s"), *AssetPath);
			return Result;
		}

		// Asset is not a mesh type we can get bounds from
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset is not a mesh type (StaticMesh or SkeletalMesh): %s"), *Asset->GetClass()->GetName()));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetBoundsTask);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FAssetService::HandleGetAssetInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	auto GetInfoTask = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Try to load the asset
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		
		if (!Asset)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Asset not found or failed to load: %s"), *AssetPath));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_name"), Asset->GetName());
		Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
		Result->SetStringField(TEXT("outer_path"), Asset->GetOuter() ? Asset->GetOuter()->GetPathName() : TEXT("None"));

		// Check if it's a StaticMesh - provide detailed info
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (StaticMesh)
		{
			Result->SetStringField(TEXT("type"), TEXT("StaticMesh"));
			
			// Bounds info
			FBox BoundingBox = StaticMesh->GetBoundingBox();
			FVector Size = BoundingBox.GetSize();
			FVector Center = BoundingBox.GetCenter();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> SizeArr, CenterArr, MinArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(BoundingBox.Min.Z));
			
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			BoundsObj->SetArrayField(TEXT("min"), MinArr);
			BoundsObj->SetNumberField(TEXT("bottom_z_offset"), -BoundingBox.Min.Z);
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			
			// Mesh stats
			Result->SetNumberField(TEXT("num_lods"), StaticMesh->GetNumLODs());
			if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = StaticMesh->GetRenderData()->LODResources[0];
				Result->SetNumberField(TEXT("num_vertices"), LOD0.GetNumVertices());
				Result->SetNumberField(TEXT("num_triangles"), LOD0.GetNumTriangles());
				Result->SetNumberField(TEXT("num_sections"), LOD0.Sections.Num());
			}
			
			// Materials
			TArray<TSharedPtr<FJsonValue>> MaterialsArr;
			for (int32 i = 0; i < StaticMesh->GetStaticMaterials().Num(); i++)
			{
				const FStaticMaterial& MatSlot = StaticMesh->GetStaticMaterials()[i];
				TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
				MatObj->SetNumberField(TEXT("index"), i);
				MatObj->SetStringField(TEXT("slot_name"), MatSlot.MaterialSlotName.ToString());
				if (MatSlot.MaterialInterface)
				{
					MatObj->SetStringField(TEXT("material_name"), MatSlot.MaterialInterface->GetName());
					MatObj->SetStringField(TEXT("material_path"), MatSlot.MaterialInterface->GetPathName());
				}
				else
				{
					MatObj->SetStringField(TEXT("material_name"), TEXT("None"));
				}
				MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
			}
			Result->SetArrayField(TEXT("materials"), MaterialsArr);
			Result->SetNumberField(TEXT("num_materials"), MaterialsArr.Num());
			
			// Collision info
			if (StaticMesh->GetBodySetup())
			{
				UBodySetup* BodySetup = StaticMesh->GetBodySetup();
				TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
				CollisionObj->SetBoolField(TEXT("has_collision"), true);
				CollisionObj->SetStringField(TEXT("collision_complexity"), 
					BodySetup->CollisionTraceFlag == CTF_UseDefault ? TEXT("Default") :
					BodySetup->CollisionTraceFlag == CTF_UseSimpleAndComplex ? TEXT("SimpleAndComplex") :
					BodySetup->CollisionTraceFlag == CTF_UseSimpleAsComplex ? TEXT("SimpleAsComplex") :
					BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple ? TEXT("ComplexAsSimple") : TEXT("Unknown")
				);
				CollisionObj->SetNumberField(TEXT("num_convex_elements"), BodySetup->AggGeom.ConvexElems.Num());
				CollisionObj->SetNumberField(TEXT("num_box_elements"), BodySetup->AggGeom.BoxElems.Num());
				CollisionObj->SetNumberField(TEXT("num_sphere_elements"), BodySetup->AggGeom.SphereElems.Num());
				CollisionObj->SetNumberField(TEXT("num_capsule_elements"), BodySetup->AggGeom.SphylElems.Num());
				Result->SetObjectField(TEXT("collision"), CollisionObj);
			}
			else
			{
				TSharedPtr<FJsonObject> CollisionObj = MakeShared<FJsonObject>();
				CollisionObj->SetBoolField(TEXT("has_collision"), false);
				Result->SetObjectField(TEXT("collision"), CollisionObj);
			}
			
			// Nanite info
			Result->SetBoolField(TEXT("nanite_enabled"), StaticMesh->NaniteSettings.bEnabled);
			
			// Lightmap info
			Result->SetNumberField(TEXT("lightmap_resolution"), StaticMesh->GetLightMapResolution());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for StaticMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a SkeletalMesh
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
		if (SkeletalMesh)
		{
			Result->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));
			
			// Bounds info
			FBox BoundingBox = SkeletalMesh->GetBounds().GetBox();
			FVector Size = BoundingBox.GetSize();
			FVector Center = BoundingBox.GetCenter();
			
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> SizeArr, CenterArr;
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.X));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Y));
			SizeArr.Add(MakeShared<FJsonValueNumber>(Size.Z));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
			CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
			
			BoundsObj->SetArrayField(TEXT("size"), SizeArr);
			BoundsObj->SetArrayField(TEXT("center"), CenterArr);
			BoundsObj->SetNumberField(TEXT("bottom_z_offset"), -BoundingBox.Min.Z);
			Result->SetObjectField(TEXT("bounds"), BoundsObj);
			
			// Skeleton info
			Result->SetNumberField(TEXT("num_bones"), SkeletalMesh->GetRefSkeleton().GetNum());
			
			// Materials
			TArray<TSharedPtr<FJsonValue>> MaterialsArr;
			for (int32 i = 0; i < SkeletalMesh->GetMaterials().Num(); i++)
			{
				const FSkeletalMaterial& MatSlot = SkeletalMesh->GetMaterials()[i];
				TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
				MatObj->SetNumberField(TEXT("index"), i);
				MatObj->SetStringField(TEXT("slot_name"), MatSlot.MaterialSlotName.ToString());
				if (MatSlot.MaterialInterface)
				{
					MatObj->SetStringField(TEXT("material_name"), MatSlot.MaterialInterface->GetName());
				}
				else
				{
					MatObj->SetStringField(TEXT("material_name"), TEXT("None"));
				}
				MaterialsArr.Add(MakeShared<FJsonValueObject>(MatObj));
			}
			Result->SetArrayField(TEXT("materials"), MaterialsArr);
			Result->SetNumberField(TEXT("num_materials"), MaterialsArr.Num());
			
			// LOD info
			Result->SetNumberField(TEXT("num_lods"), SkeletalMesh->GetLODNum());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for SkeletalMesh: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Material
		UMaterialInterface* Material = Cast<UMaterialInterface>(Asset);
		if (Material)
		{
			Result->SetStringField(TEXT("type"), TEXT("Material"));
			Result->SetStringField(TEXT("material_domain"), 
				Material->GetMaterial() ? 
					(Material->GetMaterial()->MaterialDomain == MD_Surface ? TEXT("Surface") :
					 Material->GetMaterial()->MaterialDomain == MD_DeferredDecal ? TEXT("DeferredDecal") :
					 Material->GetMaterial()->MaterialDomain == MD_LightFunction ? TEXT("LightFunction") :
					 Material->GetMaterial()->MaterialDomain == MD_PostProcess ? TEXT("PostProcess") :
					 Material->GetMaterial()->MaterialDomain == MD_UI ? TEXT("UI") : TEXT("Unknown"))
				: TEXT("Unknown")
			);
			Result->SetBoolField(TEXT("is_two_sided"), Material->IsTwoSided());
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Material: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Blueprint
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (Blueprint)
		{
			Result->SetStringField(TEXT("type"), TEXT("Blueprint"));
			Result->SetStringField(TEXT("blueprint_type"), 
				Blueprint->BlueprintType == BPTYPE_Normal ? TEXT("Normal") :
				Blueprint->BlueprintType == BPTYPE_Const ? TEXT("Const") :
				Blueprint->BlueprintType == BPTYPE_MacroLibrary ? TEXT("MacroLibrary") :
				Blueprint->BlueprintType == BPTYPE_Interface ? TEXT("Interface") :
				Blueprint->BlueprintType == BPTYPE_LevelScript ? TEXT("LevelScript") :
				Blueprint->BlueprintType == BPTYPE_FunctionLibrary ? TEXT("FunctionLibrary") : TEXT("Unknown")
			);
			if (Blueprint->ParentClass)
			{
				Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
			}
			if (Blueprint->GeneratedClass)
			{
				Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass->GetName());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Blueprint: %s"), *AssetPath);
			return Result;
		}

		// Check if it's a Texture
		UTexture* Texture = Cast<UTexture>(Asset);
		if (Texture)
		{
			Result->SetStringField(TEXT("type"), TEXT("Texture"));
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (Texture2D)
			{
				Result->SetNumberField(TEXT("width"), Texture2D->GetSizeX());
				Result->SetNumberField(TEXT("height"), Texture2D->GetSizeY());
				Result->SetNumberField(TEXT("num_mips"), Texture2D->GetNumMips());
			}
			
			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got info for Texture: %s"), *AssetPath);
			return Result;
		}

		// Generic asset - just basic info
		Result->SetStringField(TEXT("type"), TEXT("Other"));
		Result->SetStringField(TEXT("description"), TEXT("Asset loaded but type-specific info not available. Use get_properties for raw property data."));
		
		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Got basic info for asset: %s (%s)"), *AssetPath, *Asset->GetClass()->GetName());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(GetInfoTask);
	return FMCPResponse::Success(Request.Id, Result);
}

TArray<FMCPToolInfo> FAssetService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// list
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list");
		Tool.Description = TEXT("List assets in the Content Browser. Can filter by class type and path.");
		
		TSharedPtr<FJsonObject> FilterParam = MakeShared<FJsonObject>();
		FilterParam->SetStringField(TEXT("type"), TEXT("object"));
		FilterParam->SetStringField(TEXT("description"), TEXT("Optional filter object with 'class' (asset class name), 'path' (content path), and 'max_results' (limit)"));
		Tool.Parameters->SetObjectField(TEXT("filter"), FilterParam);
		
		Tools.Add(Tool);
	}
	
	// find
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("find");
		Tool.Description = TEXT("Find assets by name (partial match search).");
		
		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Asset name to search for (partial match)"));
		Tool.Parameters->SetObjectField(TEXT("name"), NameParam);
		Tool.RequiredParams.Add(TEXT("name"));
		
		Tools.Add(Tool);
	}
	
	// get_properties
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_properties");
		Tool.Description = TEXT("Get detailed properties of a specific asset by path.");
		
		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full asset path (e.g., /Game/Characters/Hero.Hero)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));
		
		Tools.Add(Tool);
	}
	
	// search
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("search");
		Tool.Description = TEXT("Search assets by query string (searches name, path, and class).");
		
		TSharedPtr<FJsonObject> QueryParam = MakeShared<FJsonObject>();
		QueryParam->SetStringField(TEXT("type"), TEXT("string"));
		QueryParam->SetStringField(TEXT("description"), TEXT("Search query string"));
		Tool.Parameters->SetObjectField(TEXT("query"), QueryParam);
		Tool.RequiredParams.Add(TEXT("query"));
		
		TSharedPtr<FJsonObject> MaxParam = MakeShared<FJsonObject>();
		MaxParam->SetStringField(TEXT("type"), TEXT("number"));
		MaxParam->SetStringField(TEXT("description"), TEXT("Maximum number of results (default: 100)"));
		Tool.Parameters->SetObjectField(TEXT("max_results"), MaxParam);
		
		Tools.Add(Tool);
	}
	
	// get_bounds
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_bounds");
		Tool.Description = TEXT("Get mesh dimensions and pivot info BEFORE spawning. Returns size, center, min, bottom_z_offset. KEY: bottom_z_offset tells you how much to ADD to spawn Z to place mesh on ground. If center != [0,0,0], pivot is offset from mesh center.");
		
		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full asset path (e.g., /Game/Meshes/MyMesh.MyMesh)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));
		
		Tools.Add(Tool);
	}
	
	// get_info
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_info");
		Tool.Description = TEXT("Get detailed asset info BEFORE placing. For meshes: bounds, materials, collision, LODs, vertex count. For blueprints: parent class, type. For textures: dimensions. Use to understand what an asset IS before spawning it.");
		
		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Full asset path (e.g., /Game/Meshes/MyMesh.MyMesh, /Game/BP/MyActor.MyActor_C)"));
		Tool.Parameters->SetObjectField(TEXT("asset_path"), PathParam);
		Tool.RequiredParams.Add(TEXT("asset_path"));
		
		Tools.Add(Tool);
	}
	
	return Tools;
}
