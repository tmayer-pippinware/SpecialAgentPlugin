// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"

#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "GameThreadDispatcher.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FUsageDescriptor
	{
		const TCHAR* Name;
		const TCHAR* Property;
		EMaterialUsage Usage;
	};

	static const FUsageDescriptor GUsageDescriptors[] =
	{
		{ TEXT("skeletal_mesh"), TEXT("bUsedWithSkeletalMesh"), MATUSAGE_SkeletalMesh },
		{ TEXT("particle_sprites"), TEXT("bUsedWithParticleSprites"), MATUSAGE_ParticleSprites },
		{ TEXT("beam_trails"), TEXT("bUsedWithBeamTrails"), MATUSAGE_BeamTrails },
		{ TEXT("mesh_particles"), TEXT("bUsedWithMeshParticles"), MATUSAGE_MeshParticles },
		{ TEXT("static_lighting"), TEXT("bUsedWithStaticLighting"), MATUSAGE_StaticLighting },
		{ TEXT("morph_targets"), TEXT("bUsedWithMorphTargets"), MATUSAGE_MorphTargets },
		{ TEXT("spline_mesh"), TEXT("bUsedWithSplineMeshes"), MATUSAGE_SplineMesh },
		{ TEXT("instanced_static_meshes"), TEXT("bUsedWithInstancedStaticMeshes"), MATUSAGE_InstancedStaticMeshes },
		{ TEXT("geometry_collections"), TEXT("bUsedWithGeometryCollections"), MATUSAGE_GeometryCollections },
		{ TEXT("clothing"), TEXT("bUsedWithClothing"), MATUSAGE_Clothing },
		{ TEXT("niagara_sprites"), TEXT("bUsedWithNiagaraSprites"), MATUSAGE_NiagaraSprites },
		{ TEXT("niagara_ribbons"), TEXT("bUsedWithNiagaraRibbons"), MATUSAGE_NiagaraRibbons },
		{ TEXT("niagara_mesh_particles"), TEXT("bUsedWithNiagaraMeshParticles"), MATUSAGE_NiagaraMeshParticles },
		{ TEXT("geometry_cache"), TEXT("bUsedWithGeometryCache"), MATUSAGE_GeometryCache },
		{ TEXT("water"), TEXT("bUsedWithWater"), MATUSAGE_Water },
		{ TEXT("hair_strands"), TEXT("bUsedWithHairStrands"), MATUSAGE_HairStrands },
		{ TEXT("lidar_point_cloud"), TEXT("bUsedWithLidarPointCloud"), MATUSAGE_LidarPointCloud },
		{ TEXT("virtual_heightfield_mesh"), TEXT("bUsedWithVirtualHeightfieldMesh"), MATUSAGE_VirtualHeightfieldMesh },
		{ TEXT("nanite"), TEXT("bUsedWithNanite"), MATUSAGE_Nanite },
		{ TEXT("voxels"), TEXT("bUsedWithVoxels"), MATUSAGE_Voxels },
		{ TEXT("volumetric_cloud"), TEXT("bUsedWithVolumetricCloud"), MATUSAGE_VolumetricCloud },
		{ TEXT("heterogeneous_volumes"), TEXT("bUsedWithHeterogeneousVolumes"), MATUSAGE_HeterogeneousVolumes },
		{ TEXT("static_mesh"), TEXT("bUsedWithStaticMesh"), MATUSAGE_StaticMesh },
	};

	static TSharedPtr<FJsonObject> MakeFailure(const FString& Error)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Error);
		return Result;
	}

	static FString NormalizeAssetPath(const FString& Input)
	{
		FString Path = Input.TrimStartAndEnd();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex))
		{
			Path = Path.Left(DotIndex);
		}
		return Path;
	}

	static FString MakeObjectPath(const FString& AssetPath)
	{
		const FString Name = FPackageName::GetShortName(AssetPath);
		return Name.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);
	}

	static bool SplitAssetPath(const FString& InputPath, FString& OutAssetPath, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid asset path: %s"), *InputPath);
			return false;
		}

		OutAssetName = FPackageName::GetShortName(OutAssetPath);
		OutPackagePath = FPackageName::GetLongPackagePath(OutAssetPath);
		if (OutAssetName.IsEmpty() || OutPackagePath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid package/name split for path: %s"), *OutAssetPath);
			return false;
		}

		return true;
	}

	template <typename TObjectType>
	static TObjectType* LoadAssetAs(const FString& AssetPath)
	{
		const FString Normalized = NormalizeAssetPath(AssetPath);
		if (Normalized.IsEmpty())
		{
			return nullptr;
		}

		if (UObject* LoadedByLibrary = UEditorAssetLibrary::LoadAsset(Normalized))
		{
			if (TObjectType* Typed = Cast<TObjectType>(LoadedByLibrary))
			{
				return Typed;
			}
		}

		const FString ObjectPath = MakeObjectPath(Normalized);
		return ObjectPath.IsEmpty() ? nullptr : LoadObject<TObjectType>(nullptr, *ObjectPath);
	}

	static const FUsageDescriptor* FindUsageDescriptor(const FString& Name)
	{
		for (const FUsageDescriptor& Descriptor : GUsageDescriptors)
		{
			if (Name.Equals(Descriptor.Name, ESearchCase::IgnoreCase))
			{
				return &Descriptor;
			}
		}
		return nullptr;
	}

	static FString DomainToString(EMaterialDomain Domain)
	{
		switch (Domain)
		{
		case MD_Surface: return TEXT("surface");
		case MD_DeferredDecal: return TEXT("deferred_decal");
		case MD_LightFunction: return TEXT("light_function");
		case MD_Volume: return TEXT("volume");
		case MD_PostProcess: return TEXT("post_process");
		case MD_UI: return TEXT("ui");
		default: return TEXT("unknown");
		}
	}

	static FString BlendModeToString(EBlendMode BlendMode)
	{
		switch (BlendMode)
		{
		case BLEND_Opaque: return TEXT("opaque");
		case BLEND_Masked: return TEXT("masked");
		case BLEND_Translucent: return TEXT("translucent");
		case BLEND_Additive: return TEXT("additive");
		case BLEND_Modulate: return TEXT("modulate");
		case BLEND_AlphaComposite: return TEXT("alpha_composite");
		case BLEND_AlphaHoldout: return TEXT("alpha_holdout");
		case BLEND_TranslucentColoredTransmittance: return TEXT("translucent_colored_transmittance");
		default: return TEXT("unknown");
		}
	}

	static FString ShadingModelToString(EMaterialShadingModel ShadingModel)
	{
		switch (ShadingModel)
		{
		case MSM_Unlit: return TEXT("unlit");
		case MSM_DefaultLit: return TEXT("default_lit");
		case MSM_Subsurface: return TEXT("subsurface");
		case MSM_PreintegratedSkin: return TEXT("preintegrated_skin");
		case MSM_ClearCoat: return TEXT("clear_coat");
		case MSM_SubsurfaceProfile: return TEXT("subsurface_profile");
		case MSM_TwoSidedFoliage: return TEXT("two_sided_foliage");
		case MSM_Hair: return TEXT("hair");
		case MSM_Cloth: return TEXT("cloth");
		case MSM_Eye: return TEXT("eye");
		case MSM_SingleLayerWater: return TEXT("single_layer_water");
		case MSM_ThinTranslucent: return TEXT("thin_translucent");
		case MSM_Strata: return TEXT("substrate");
		case MSM_FromMaterialExpression: return TEXT("from_material_expression");
		default: return TEXT("unknown");
		}
	}

	static bool ParseDomain(const FString& Value, EMaterialDomain& OutDomain)
	{
		int32 NumericValue = 0;
		if (FDefaultValueHelper::ParseInt(Value, NumericValue) && NumericValue >= 0 && NumericValue < MD_MAX)
		{
			OutDomain = static_cast<EMaterialDomain>(NumericValue);
			return true;
		}
		if (Value.Equals(TEXT("surface"), ESearchCase::IgnoreCase)) { OutDomain = MD_Surface; return true; }
		if (Value.Equals(TEXT("deferred_decal"), ESearchCase::IgnoreCase)) { OutDomain = MD_DeferredDecal; return true; }
		if (Value.Equals(TEXT("light_function"), ESearchCase::IgnoreCase)) { OutDomain = MD_LightFunction; return true; }
		if (Value.Equals(TEXT("volume"), ESearchCase::IgnoreCase)) { OutDomain = MD_Volume; return true; }
		if (Value.Equals(TEXT("post_process"), ESearchCase::IgnoreCase)) { OutDomain = MD_PostProcess; return true; }
		if (Value.Equals(TEXT("ui"), ESearchCase::IgnoreCase)) { OutDomain = MD_UI; return true; }
		return false;
	}

	static bool ParseBlendMode(const FString& Value, EBlendMode& OutBlendMode)
	{
		int32 NumericValue = 0;
		if (FDefaultValueHelper::ParseInt(Value, NumericValue) && NumericValue >= 0 && NumericValue < BLEND_MAX)
		{
			OutBlendMode = static_cast<EBlendMode>(NumericValue);
			return true;
		}
		if (Value.Equals(TEXT("opaque"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Opaque; return true; }
		if (Value.Equals(TEXT("masked"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Masked; return true; }
		if (Value.Equals(TEXT("translucent"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Translucent; return true; }
		if (Value.Equals(TEXT("additive"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Additive; return true; }
		if (Value.Equals(TEXT("modulate"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Modulate; return true; }
		if (Value.Equals(TEXT("alpha_composite"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_AlphaComposite; return true; }
		if (Value.Equals(TEXT("alpha_holdout"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_AlphaHoldout; return true; }
		if (Value.Equals(TEXT("translucent_colored_transmittance"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_TranslucentColoredTransmittance; return true; }
		return false;
	}

	static bool ParseShadingModel(const FString& Value, EMaterialShadingModel& OutShadingModel)
	{
		int32 NumericValue = 0;
		if (FDefaultValueHelper::ParseInt(Value, NumericValue) && NumericValue >= 0 && NumericValue < MSM_MAX)
		{
			OutShadingModel = static_cast<EMaterialShadingModel>(NumericValue);
			return true;
		}
		if (Value.Equals(TEXT("unlit"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Unlit; return true; }
		if (Value.Equals(TEXT("default_lit"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_DefaultLit; return true; }
		if (Value.Equals(TEXT("subsurface"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Subsurface; return true; }
		if (Value.Equals(TEXT("preintegrated_skin"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_PreintegratedSkin; return true; }
		if (Value.Equals(TEXT("clear_coat"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_ClearCoat; return true; }
		if (Value.Equals(TEXT("subsurface_profile"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_SubsurfaceProfile; return true; }
		if (Value.Equals(TEXT("two_sided_foliage"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_TwoSidedFoliage; return true; }
		if (Value.Equals(TEXT("hair"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Hair; return true; }
		if (Value.Equals(TEXT("cloth"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Cloth; return true; }
		if (Value.Equals(TEXT("eye"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Eye; return true; }
		if (Value.Equals(TEXT("single_layer_water"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_SingleLayerWater; return true; }
		if (Value.Equals(TEXT("thin_translucent"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_ThinTranslucent; return true; }
		if (Value.Equals(TEXT("substrate"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_Strata; return true; }
		if (Value.Equals(TEXT("from_material_expression"), ESearchCase::IgnoreCase)) { OutShadingModel = MSM_FromMaterialExpression; return true; }
		return false;
	}

	static void WriteMaterialSettings(const UMaterial* Material, const TSharedPtr<FJsonObject>& Result)
	{
		Result->SetStringField(TEXT("domain"), DomainToString(Material->MaterialDomain));
		Result->SetStringField(TEXT("blend_mode"), BlendModeToString(Material->BlendMode));
		Result->SetBoolField(TEXT("two_sided"), Material->TwoSided != 0);

		const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
		Result->SetStringField(TEXT("shading_model"), TEXT("unknown"));
		for (int32 Index = 0; Index < static_cast<int32>(MSM_NUM); ++Index)
		{
			const EMaterialShadingModel Model = static_cast<EMaterialShadingModel>(Index);
			if (ShadingModels.HasShadingModel(Model))
			{
				Result->SetStringField(TEXT("shading_model"), ShadingModelToString(Model));
				break;
			}
		}

		TSharedPtr<FJsonObject> UsageObj = MakeShared<FJsonObject>();
		for (const FUsageDescriptor& Descriptor : GUsageDescriptors)
		{
			if (const FBoolProperty* BoolProperty = FindFProperty<FBoolProperty>(UMaterial::StaticClass(), FName(Descriptor.Property)))
			{
				UsageObj->SetBoolField(Descriptor.Name, BoolProperty->GetPropertyValue_InContainer(Material));
			}
		}
		Result->SetObjectField(TEXT("usage_flags"), UsageObj);
	}
}

FMaterialService::FMaterialService()
{
}

FString FMaterialService::GetServiceDescription() const
{
	return TEXT("Material authoring - lifecycle, metadata, and settings management");
}

TArray<FMCPToolInfo> FMaterialService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	auto AddTool = [&Tools](const TCHAR* Name, const TCHAR* Description)
	{
		FMCPToolInfo Tool;
		Tool.Name = Name;
		Tool.Description = Description;
		Tools.Add(Tool);
	};

	AddTool(TEXT("create_material"), TEXT("Create a new material asset."));
	AddTool(TEXT("create_material_instance"), TEXT("Create a new material instance asset."));
	AddTool(TEXT("create_material_function"), TEXT("Create a new material function asset."));
	AddTool(TEXT("create_parameter_collection"), TEXT("Create a new material parameter collection asset."));
	AddTool(TEXT("duplicate_asset"), TEXT("Duplicate a material-related asset."));
	AddTool(TEXT("rename_asset"), TEXT("Rename (move) a material-related asset."));
	AddTool(TEXT("delete_asset"), TEXT("Delete a material-related asset."));
	AddTool(TEXT("save_asset"), TEXT("Save a material-related asset."));
	AddTool(TEXT("get_material_info"), TEXT("Get details for a material-related asset."));
	AddTool(TEXT("set_material_settings"), TEXT("Set domain/blend/shading/two-sided/usage flags for a material."));
	AddTool(TEXT("capabilities"), TEXT("Report baseline material service capabilities and module availability."));
	return Tools;
}

FMCPResponse FMaterialService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("create_material")) return HandleCreateMaterial(Request);
	if (MethodName == TEXT("create_material_instance")) return HandleCreateMaterialInstance(Request);
	if (MethodName == TEXT("create_material_function")) return HandleCreateMaterialFunction(Request);
	if (MethodName == TEXT("create_parameter_collection")) return HandleCreateParameterCollection(Request);
	if (MethodName == TEXT("duplicate_asset")) return HandleDuplicateAsset(Request);
	if (MethodName == TEXT("rename_asset")) return HandleRenameAsset(Request);
	if (MethodName == TEXT("delete_asset")) return HandleDeleteAsset(Request);
	if (MethodName == TEXT("save_asset")) return HandleSaveAsset(Request);
	if (MethodName == TEXT("get_material_info")) return HandleGetMaterialInfo(Request);
	if (MethodName == TEXT("set_material_settings")) return HandleSetMaterialSettings(Request);
	if (MethodName == TEXT("capabilities")) return HandleCapabilities(Request);
	return MethodNotFound(Request.Id, TEXT("material"), MethodName);
}

FMCPResponse FMaterialService::HandleCreateMaterial(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString MaterialPath;
	if (!Request.Params->TryGetStringField(TEXT("material_path"), MaterialPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_path'"));
	}

	auto Task = [MaterialPath]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath, PackagePath, AssetName, Error;
		if (!SplitAssetPath(MaterialPath, AssetPath, PackagePath, AssetName, Error)) return MakeFailure(Error);
		if (UEditorAssetLibrary::DoesAssetExist(AssetPath)) return MakeFailure(FString::Printf(TEXT("Asset already exists: %s"), *AssetPath));

		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UMaterial* Material = Cast<UMaterial>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory, FName(TEXT("SpecialAgent"))));
		if (!Material) return MakeFailure(FString::Printf(TEXT("Failed to create material: %s"), *AssetPath));

		Material->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Material->GetClass()->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCreateMaterialInstance(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString InstancePath;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), InstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}

	FString ParentPath;
	Request.Params->TryGetStringField(TEXT("parent_material_path"), ParentPath);

	auto Task = [InstancePath, ParentPath]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath, PackagePath, AssetName, Error;
		if (!SplitAssetPath(InstancePath, AssetPath, PackagePath, AssetName, Error)) return MakeFailure(Error);
		if (UEditorAssetLibrary::DoesAssetExist(AssetPath)) return MakeFailure(FString::Printf(TEXT("Asset already exists: %s"), *AssetPath));

		UMaterialInterface* Parent = nullptr;
		if (!ParentPath.TrimStartAndEnd().IsEmpty())
		{
			Parent = LoadAssetAs<UMaterialInterface>(ParentPath);
			if (!Parent) return MakeFailure(FString::Printf(TEXT("Parent material not found: %s"), *ParentPath));
		}

		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Parent;
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory, FName(TEXT("SpecialAgent"))));
		if (!Instance) return MakeFailure(FString::Printf(TEXT("Failed to create material instance: %s"), *AssetPath));

		Instance->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Instance->GetClass()->GetPathName());
		Result->SetStringField(TEXT("parent_material"), Instance->Parent ? Instance->Parent->GetPathName() : TEXT(""));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCreateMaterialFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	auto Task = [FunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath, PackagePath, AssetName, Error;
		if (!SplitAssetPath(FunctionPath, AssetPath, PackagePath, AssetName, Error)) return MakeFailure(Error);
		if (UEditorAssetLibrary::DoesAssetExist(AssetPath)) return MakeFailure(FString::Printf(TEXT("Asset already exists: %s"), *AssetPath));

		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
		UMaterialFunction* Function = Cast<UMaterialFunction>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), Factory, FName(TEXT("SpecialAgent"))));
		if (!Function) return MakeFailure(FString::Printf(TEXT("Failed to create material function: %s"), *AssetPath));

		Function->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Function->GetClass()->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCreateParameterCollection(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	auto Task = [CollectionPath]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath, PackagePath, AssetName, Error;
		if (!SplitAssetPath(CollectionPath, AssetPath, PackagePath, AssetName, Error)) return MakeFailure(Error);
		if (UEditorAssetLibrary::DoesAssetExist(AssetPath)) return MakeFailure(FString::Printf(TEXT("Asset already exists: %s"), *AssetPath));

		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UMaterialParameterCollectionFactoryNew* Factory = NewObject<UMaterialParameterCollectionFactoryNew>();
		UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(AssetTools.Get().CreateAsset(AssetName, PackagePath, UMaterialParameterCollection::StaticClass(), Factory, FName(TEXT("SpecialAgent"))));
		if (!Collection) return MakeFailure(FString::Printf(TEXT("Failed to create parameter collection: %s"), *AssetPath));

		Collection->MarkPackageDirty();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetStringField(TEXT("asset_class"), Collection->GetClass()->GetPathName());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDuplicateAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString SourcePath;
	FString DestinationPath;
	if (!Request.Params->TryGetStringField(TEXT("source_asset_path"), SourcePath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'source_asset_path'"));
	if (!Request.Params->TryGetStringField(TEXT("destination_asset_path"), DestinationPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'destination_asset_path'"));

	auto Task = [SourcePath, DestinationPath]() -> TSharedPtr<FJsonObject>
	{
		const FString SourceAssetPath = NormalizeAssetPath(SourcePath);
		const FString DestinationAssetPath = NormalizeAssetPath(DestinationPath);
		if (!FPackageName::IsValidLongPackageName(SourceAssetPath) || !FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			return MakeFailure(TEXT("Invalid source or destination asset path"));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(SourceAssetPath)) return MakeFailure(FString::Printf(TEXT("Source asset not found: %s"), *SourceAssetPath));
		if (UEditorAssetLibrary::DoesAssetExist(DestinationAssetPath)) return MakeFailure(FString::Printf(TEXT("Destination already exists: %s"), *DestinationAssetPath));
		if (!UEditorAssetLibrary::DuplicateAsset(SourceAssetPath, DestinationAssetPath))
		{
			return MakeFailure(FString::Printf(TEXT("Failed to duplicate asset from %s to %s"), *SourceAssetPath, *DestinationAssetPath));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("source_asset_path"), SourceAssetPath);
		Result->SetStringField(TEXT("destination_asset_path"), DestinationAssetPath);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRenameAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString AssetPath;
	FString NewAssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	if (!Request.Params->TryGetStringField(TEXT("new_asset_path"), NewAssetPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_asset_path'"));

	auto Task = [AssetPath, NewAssetPath]() -> TSharedPtr<FJsonObject>
	{
		const FString SourceAssetPath = NormalizeAssetPath(AssetPath);
		const FString DestinationAssetPath = NormalizeAssetPath(NewAssetPath);
		if (!FPackageName::IsValidLongPackageName(SourceAssetPath) || !FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			return MakeFailure(TEXT("Invalid source or destination asset path"));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(SourceAssetPath)) return MakeFailure(FString::Printf(TEXT("Asset not found: %s"), *SourceAssetPath));
		if (UEditorAssetLibrary::DoesAssetExist(DestinationAssetPath)) return MakeFailure(FString::Printf(TEXT("Destination already exists: %s"), *DestinationAssetPath));
		if (!UEditorAssetLibrary::RenameAsset(SourceAssetPath, DestinationAssetPath))
		{
			return MakeFailure(FString::Printf(TEXT("Failed to rename asset from %s to %s"), *SourceAssetPath, *DestinationAssetPath));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("old_asset_path"), SourceAssetPath);
		Result->SetStringField(TEXT("new_asset_path"), DestinationAssetPath);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDeleteAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		const FString NormalizedPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedPath)) return MakeFailure(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
		if (!UEditorAssetLibrary::DoesAssetExist(NormalizedPath)) return MakeFailure(FString::Printf(TEXT("Asset not found: %s"), *NormalizedPath));
		if (!UEditorAssetLibrary::DeleteAsset(NormalizedPath)) return MakeFailure(FString::Printf(TEXT("Failed to delete asset: %s"), *NormalizedPath));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), NormalizedPath);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSaveAsset(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString AssetPath;
	bool bOnlyIfDirty = true;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	Request.Params->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);

	auto Task = [AssetPath, bOnlyIfDirty]() -> TSharedPtr<FJsonObject>
	{
		const FString NormalizedPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedPath)) return MakeFailure(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
		if (!UEditorAssetLibrary::DoesAssetExist(NormalizedPath)) return MakeFailure(FString::Printf(TEXT("Asset not found: %s"), *NormalizedPath));
		if (!UEditorAssetLibrary::SaveAsset(NormalizedPath, bOnlyIfDirty)) return MakeFailure(FString::Printf(TEXT("Failed to save asset: %s"), *NormalizedPath));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), NormalizedPath);
		Result->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleGetMaterialInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		UObject* Asset = LoadAssetAs<UObject>(AssetPath);
		if (!Asset) return MakeFailure(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Asset->GetPathName()));
		Result->SetStringField(TEXT("asset_name"), Asset->GetName());
		Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
		Result->SetStringField(TEXT("package_name"), Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : TEXT(""));

		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			Result->SetStringField(TEXT("material_asset_type"), TEXT("material"));
			WriteMaterialSettings(Material, Result);
			return Result;
		}
		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset))
		{
			Result->SetStringField(TEXT("material_asset_type"), TEXT("material_instance"));
			Result->SetStringField(TEXT("parent_material"), Instance->Parent ? Instance->Parent->GetPathName() : TEXT(""));
			Result->SetNumberField(TEXT("scalar_override_count"), Instance->ScalarParameterValues.Num());
			Result->SetNumberField(TEXT("vector_override_count"), Instance->VectorParameterValues.Num());
			Result->SetNumberField(TEXT("texture_override_count"), Instance->TextureParameterValues.Num());
			return Result;
		}
		if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
		{
			Result->SetStringField(TEXT("material_asset_type"), TEXT("material_function"));
			Result->SetStringField(TEXT("description"), Function->Description);
			Result->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary != 0);
			return Result;
		}
		if (UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Asset))
		{
			Result->SetStringField(TEXT("material_asset_type"), TEXT("parameter_collection"));
			Result->SetNumberField(TEXT("scalar_parameter_count"), Collection->ScalarParameters.Num());
			Result->SetNumberField(TEXT("vector_parameter_count"), Collection->VectorParameters.Num());
			return Result;
		}

		return MakeFailure(FString::Printf(TEXT("Unsupported asset type: %s"), *Asset->GetClass()->GetPathName()));
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetMaterialSettings(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid()) return InvalidParams(Request.Id, TEXT("Missing params object"));

	FString AssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	const bool bHasDomain = Request.Params->HasField(TEXT("domain"));
	const bool bHasBlendMode = Request.Params->HasField(TEXT("blend_mode"));
	const bool bHasShadingModel = Request.Params->HasField(TEXT("shading_model"));
	const bool bHasTwoSided = Request.Params->HasField(TEXT("two_sided"));
	const bool bHasUsageFlags = Request.Params->HasField(TEXT("usage_flags"));

	if (!bHasDomain && !bHasBlendMode && !bHasShadingModel && !bHasTwoSided && !bHasUsageFlags)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one setting: domain, blend_mode, shading_model, two_sided, usage_flags"));
	}

	EMaterialDomain Domain = MD_Surface;
	if (bHasDomain)
	{
		FString Value;
		if (!Request.Params->TryGetStringField(TEXT("domain"), Value) || !ParseDomain(Value, Domain))
		{
			return InvalidParams(Request.Id, TEXT("Invalid 'domain'"));
		}
	}

	EBlendMode BlendMode = BLEND_Opaque;
	if (bHasBlendMode)
	{
		FString Value;
		if (!Request.Params->TryGetStringField(TEXT("blend_mode"), Value) || !ParseBlendMode(Value, BlendMode))
		{
			return InvalidParams(Request.Id, TEXT("Invalid 'blend_mode'"));
		}
	}

	EMaterialShadingModel ShadingModel = MSM_DefaultLit;
	if (bHasShadingModel)
	{
		FString Value;
		if (!Request.Params->TryGetStringField(TEXT("shading_model"), Value) || !ParseShadingModel(Value, ShadingModel))
		{
			return InvalidParams(Request.Id, TEXT("Invalid 'shading_model'"));
		}
	}

	bool bTwoSided = false;
	if (bHasTwoSided && !Request.Params->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		return InvalidParams(Request.Id, TEXT("'two_sided' must be a boolean"));
	}

	TMap<FString, bool> UsageFlags;
	if (bHasUsageFlags)
	{
		const TSharedPtr<FJsonObject>* UsageFlagsObject = nullptr;
		if (!Request.Params->TryGetObjectField(TEXT("usage_flags"), UsageFlagsObject))
		{
			return InvalidParams(Request.Id, TEXT("'usage_flags' must be an object of { flag_name: bool }"));
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*UsageFlagsObject)->Values)
		{
			const FUsageDescriptor* Descriptor = FindUsageDescriptor(Entry.Key);
			if (!Descriptor) return InvalidParams(Request.Id, FString::Printf(TEXT("Unknown usage flag: %s"), *Entry.Key));

			bool bEnabled = false;
			if (!Entry.Value.IsValid() || !Entry.Value->TryGetBool(bEnabled))
			{
				return InvalidParams(Request.Id, FString::Printf(TEXT("Usage flag '%s' must be a boolean"), *Entry.Key));
			}
			UsageFlags.Add(Descriptor->Name, bEnabled);
		}
	}

	auto Task = [AssetPath, bHasDomain, Domain, bHasBlendMode, BlendMode, bHasShadingModel, ShadingModel, bHasTwoSided, bTwoSided, UsageFlags]() -> TSharedPtr<FJsonObject>
	{
		UMaterial* Material = LoadAssetAs<UMaterial>(AssetPath);
		if (!Material) return MakeFailure(FString::Printf(TEXT("Material not found: %s"), *AssetPath));

		bool bChanged = false;
		bool bNeedsRecompile = false;
		Material->Modify();

		if (bHasDomain && Material->MaterialDomain != Domain) { Material->MaterialDomain = Domain; bChanged = true; }
		if (bHasBlendMode && Material->BlendMode != BlendMode) { Material->BlendMode = BlendMode; bChanged = true; }
		if (bHasShadingModel && !Material->GetShadingModels().HasOnlyShadingModel(ShadingModel)) { Material->SetShadingModel(ShadingModel); bChanged = true; }
		if (bHasTwoSided && (Material->TwoSided != 0) != bTwoSided) { Material->TwoSided = bTwoSided ? 1 : 0; bChanged = true; }

		for (const TPair<FString, bool>& Entry : UsageFlags)
		{
			const FUsageDescriptor* Descriptor = FindUsageDescriptor(Entry.Key);
			if (!Descriptor) continue;

			if (FBoolProperty* BoolProperty = FindFProperty<FBoolProperty>(UMaterial::StaticClass(), FName(Descriptor->Property)))
			{
				const bool bCurrentValue = BoolProperty->GetPropertyValue_InContainer(Material);
				if (bCurrentValue != Entry.Value)
				{
					BoolProperty->SetPropertyValue_InContainer(Material, Entry.Value);
					bChanged = true;
				}
			}

			if (Entry.Value)
			{
				bool bUsageNeedsRecompile = false;
				Material->SetMaterialUsage(bUsageNeedsRecompile, Descriptor->Usage);
				bNeedsRecompile = bNeedsRecompile || bUsageNeedsRecompile;
			}
		}

		if (bChanged)
		{
			Material->PostEditChange();
			Material->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetBoolField(TEXT("needs_recompile"), bNeedsRecompile);
		Result->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Material->GetPathName()));
		WriteMaterialSettings(Material, Result);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCapabilities(const FMCPRequest& Request)
{
	auto Task = []() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("service"), TEXT("material"));
		Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

		TSharedPtr<FJsonObject> BaselineObj = MakeShared<FJsonObject>();
		BaselineObj->SetBoolField(TEXT("registered_in_router"), true);
		BaselineObj->SetBoolField(TEXT("module_dependencies_declared"), true);
		Result->SetObjectField(TEXT("baseline"), BaselineObj);

		TSharedPtr<FJsonObject> PhasesObj = MakeShared<FJsonObject>();
		PhasesObj->SetBoolField(TEXT("phase_1_asset_class_lifecycle"), true);
		Result->SetObjectField(TEXT("phases"), PhasesObj);

		TSharedPtr<FJsonObject> DependenciesObj = MakeShared<FJsonObject>();
		DependenciesObj->SetBoolField(TEXT("material_editor_module_exists"), FModuleManager::Get().ModuleExists(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("material_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("asset_registry_module_exists"), FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")));
		DependenciesObj->SetBoolField(TEXT("asset_tools_module_exists"), FModuleManager::Get().ModuleExists(TEXT("AssetTools")));
		DependenciesObj->SetBoolField(TEXT("asset_tools_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")));
		DependenciesObj->SetBoolField(TEXT("unreal_ed_module_exists"), FModuleManager::Get().ModuleExists(TEXT("UnrealEd")));
		DependenciesObj->SetBoolField(TEXT("editor_scripting_utilities_module_exists"), FModuleManager::Get().ModuleExists(TEXT("EditorScriptingUtilities")));
		Result->SetObjectField(TEXT("dependencies"), DependenciesObj);

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}
