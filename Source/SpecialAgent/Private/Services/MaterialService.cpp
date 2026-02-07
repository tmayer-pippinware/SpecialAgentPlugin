// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"

#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "GameThreadDispatcher.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
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

	struct FMaterialGraphContext
	{
		UMaterial* Material = nullptr;
		UMaterialFunction* MaterialFunction = nullptr;
		FString AssetPath;

		bool IsValid() const
		{
			return Material != nullptr || MaterialFunction != nullptr;
		}

		UObject* GetOuter() const
		{
			return Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(MaterialFunction);
		}

		void MarkDirty() const
		{
			if (Material)
			{
				Material->MarkPackageDirty();
			}
			if (MaterialFunction)
			{
				MaterialFunction->MarkPackageDirty();
			}
		}
	};

	static bool ResolveGraphContext(const FString& InputPath, FMaterialGraphContext& OutContext, FString& OutError)
	{
		const FString AssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid asset path: %s"), *InputPath);
			return false;
		}

		if (UMaterial* Material = LoadAssetAs<UMaterial>(AssetPath))
		{
			OutContext.Material = Material;
			OutContext.MaterialFunction = nullptr;
			OutContext.AssetPath = AssetPath;
			return true;
		}

		if (UMaterialFunction* MaterialFunction = LoadAssetAs<UMaterialFunction>(AssetPath))
		{
			OutContext.Material = nullptr;
			OutContext.MaterialFunction = MaterialFunction;
			OutContext.AssetPath = AssetPath;
			return true;
		}

		OutError = FString::Printf(TEXT("Asset is not a material or material function: %s"), *AssetPath);
		return false;
	}

	static void GatherGraphNodes(const FMaterialGraphContext& Context, TArray<UMaterialExpression*>& OutExpressions, TArray<UMaterialExpressionComment*>& OutComments)
	{
		OutExpressions.Reset();
		OutComments.Reset();

		if (Context.Material)
		{
			for (UMaterialExpression* Expression : Context.Material->GetExpressions())
			{
				if (Expression)
				{
					OutExpressions.Add(Expression);
				}
			}
			for (UMaterialExpressionComment* Comment : Context.Material->GetEditorComments())
			{
				if (Comment)
				{
					OutComments.Add(Comment);
				}
			}
		}
		else if (Context.MaterialFunction)
		{
			for (UMaterialExpression* Expression : Context.MaterialFunction->GetExpressions())
			{
				if (Expression)
				{
					OutExpressions.Add(Expression);
				}
			}
			for (UMaterialExpressionComment* Comment : Context.MaterialFunction->GetEditorComments())
			{
				if (Comment)
				{
					OutComments.Add(Comment);
				}
			}
		}
	}

	static FString GetNodeId(const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return FString();
		}

		if (Expression->MaterialExpressionGuid.IsValid())
		{
			return Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphens);
		}

		return Expression->GetName();
	}

	static UMaterialExpression* FindNodeById(const FMaterialGraphContext& Context, const FString& NodeId)
	{
		const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
		if (TrimmedNodeId.IsEmpty())
		{
			return nullptr;
		}

		FGuid ParsedGuid;
		const bool bHasGuid = FGuid::Parse(TrimmedNodeId, ParsedGuid);

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression)
			{
				continue;
			}
			if (bHasGuid && Expression->MaterialExpressionGuid == ParsedGuid)
			{
				return Expression;
			}
			if (Expression->GetName().Equals(TrimmedNodeId, ESearchCase::IgnoreCase))
			{
				return Expression;
			}
		}

		for (UMaterialExpressionComment* Comment : Comments)
		{
			if (!Comment)
			{
				continue;
			}
			if (bHasGuid && Comment->MaterialExpressionGuid == ParsedGuid)
			{
				return Comment;
			}
			if (Comment->GetName().Equals(TrimmedNodeId, ESearchCase::IgnoreCase))
			{
				return Comment;
			}
		}

		return nullptr;
	}

	static UClass* ResolveExpressionClass(const FString& ClassNameOrPath)
	{
		const FString Trimmed = ClassNameOrPath.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *Trimmed))
		{
			return Existing->IsChildOf(UMaterialExpression::StaticClass()) ? Existing : nullptr;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *Trimmed))
		{
			return Loaded->IsChildOf(UMaterialExpression::StaticClass()) ? Loaded : nullptr;
		}

		const FString PrefixedName = Trimmed.StartsWith(TEXT("MaterialExpression")) ? Trimmed : FString::Printf(TEXT("MaterialExpression%s"), *Trimmed);
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate || !Candidate->IsChildOf(UMaterialExpression::StaticClass()))
			{
				continue;
			}
			if (Candidate->GetName().Equals(Trimmed, ESearchCase::IgnoreCase) ||
				Candidate->GetName().Equals(PrefixedName, ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildNodeJson(const UMaterialExpression* Expression)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		if (!Expression)
		{
			Node->SetBoolField(TEXT("valid"), false);
			return Node;
		}

		Node->SetBoolField(TEXT("valid"), true);
		Node->SetStringField(TEXT("node_id"), GetNodeId(Expression));
		Node->SetStringField(TEXT("name"), Expression->GetName());
		Node->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
		Node->SetStringField(TEXT("class_path"), Expression->GetClass()->GetPathName());
		Node->SetStringField(TEXT("description"), Expression->GetDescription());
		Node->SetNumberField(TEXT("node_pos_x"), Expression->MaterialExpressionEditorX);
		Node->SetNumberField(TEXT("node_pos_y"), Expression->MaterialExpressionEditorY);
		Node->SetBoolField(TEXT("is_comment"), Expression->IsA<UMaterialExpressionComment>());
		Node->SetBoolField(TEXT("is_reroute"), Expression->IsA<UMaterialExpressionReroute>());

		if (const UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expression))
		{
			Node->SetStringField(TEXT("text"), Comment->Text);
			Node->SetNumberField(TEXT("size_x"), Comment->SizeX);
			Node->SetNumberField(TEXT("size_y"), Comment->SizeY);
		}

		return Node;
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
	AddTool(TEXT("list_nodes"), TEXT("List graph nodes for a material or material function."));
	AddTool(TEXT("add_expression_by_class"), TEXT("Add an expression node by class."));
	AddTool(TEXT("delete_node"), TEXT("Delete a graph node by node id."));
	AddTool(TEXT("duplicate_node"), TEXT("Duplicate a graph node."));
	AddTool(TEXT("move_node"), TEXT("Move a graph node to an editor position."));
	AddTool(TEXT("add_comment_node"), TEXT("Add a comment node."));
	AddTool(TEXT("add_reroute_node"), TEXT("Add a reroute node."));
	AddTool(TEXT("layout_graph"), TEXT("Auto-layout graph nodes."));
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
	if (MethodName == TEXT("list_nodes")) return HandleListNodes(Request);
	if (MethodName == TEXT("add_expression_by_class")) return HandleAddExpressionByClass(Request);
	if (MethodName == TEXT("delete_node")) return HandleDeleteNode(Request);
	if (MethodName == TEXT("duplicate_node")) return HandleDuplicateNode(Request);
	if (MethodName == TEXT("move_node")) return HandleMoveNode(Request);
	if (MethodName == TEXT("add_comment_node")) return HandleAddCommentNode(Request);
	if (MethodName == TEXT("add_reroute_node")) return HandleAddRerouteNode(Request);
	if (MethodName == TEXT("layout_graph")) return HandleLayoutGraph(Request);
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

FMCPResponse FMaterialService::HandleListNodes(const FMCPRequest& Request)
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

	bool bIncludeComments = true;
	Request.Params->TryGetBoolField(TEXT("include_comments"), bIncludeComments);

	auto Task = [AssetPath, bIncludeComments]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UMaterialExpression* Expression : Expressions)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Expression)));
		}
		if (bIncludeComments)
		{
			for (UMaterialExpressionComment* Comment : Comments)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Comment)));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("asset_type"), Context.Material ? TEXT("material") : TEXT("material_function"));
		Result->SetArrayField(TEXT("nodes"), Nodes);
		Result->SetNumberField(TEXT("node_count"), Nodes.Num());
		Result->SetNumberField(TEXT("expression_count"), Expressions.Num());
		Result->SetNumberField(TEXT("comment_count"), Comments.Num());
		Result->SetBoolField(TEXT("include_comments"), bIncludeComments);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleAddExpressionByClass(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString ExpressionClassName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("expression_class"), ExpressionClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'expression_class'"));
	}

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	double NodePosXDouble = 0.0;
	double NodePosYDouble = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NodePosXDouble))
	{
		NodePosX = static_cast<int32>(NodePosXDouble);
	}
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NodePosYDouble))
	{
		NodePosY = static_cast<int32>(NodePosYDouble);
	}

	FString SelectedAssetPath;
	Request.Params->TryGetStringField(TEXT("selected_asset_path"), SelectedAssetPath);

	auto Task = [AssetPath, ExpressionClassName, NodePosX, NodePosY, SelectedAssetPath]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UClass* ExpressionClass = ResolveExpressionClass(ExpressionClassName);
		if (!ExpressionClass || !ExpressionClass->IsChildOf(UMaterialExpression::StaticClass()))
		{
			return MakeFailure(FString::Printf(TEXT("Expression class not found: %s"), *ExpressionClassName));
		}
		if (ExpressionClass->IsChildOf(UMaterialExpressionComment::StaticClass()))
		{
			return MakeFailure(TEXT("Use material/add_comment_node for comment nodes"));
		}

		const UMaterialExpression* CDO = Cast<UMaterialExpression>(ExpressionClass->GetDefaultObject());
		if (!CDO || !CDO->IsAllowedIn(Context.GetOuter()))
		{
			return MakeFailure(FString::Printf(TEXT("Expression class is not allowed in this graph: %s"), *ExpressionClass->GetPathName()));
		}

		UObject* SelectedAsset = nullptr;
		if (!SelectedAssetPath.TrimStartAndEnd().IsEmpty())
		{
			SelectedAsset = UEditorAssetLibrary::LoadAsset(NormalizeAssetPath(SelectedAssetPath));
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Context.Material,
			Context.MaterialFunction,
			ExpressionClass,
			SelectedAsset,
			NodePosX,
			NodePosY,
			true
		);
		if (!NewExpression)
		{
			return MakeFailure(TEXT("Failed to create material expression"));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(NewExpression));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDeleteNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString NodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}

	auto Task = [AssetPath, NodeId]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* ExistingNode = FindNodeById(Context, NodeId);
		if (!ExistingNode)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *NodeId));
		}

		if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(ExistingNode))
		{
			if (Context.Material)
			{
				Context.Material->GetExpressionCollection().RemoveComment(Comment);
			}
			else if (Context.MaterialFunction)
			{
				Context.MaterialFunction->GetExpressionCollection().RemoveComment(Comment);
			}
			Comment->MarkAsGarbage();
			Context.MarkDirty();
		}
		else if (Context.Material)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, ExistingNode);
		}
		else if (Context.MaterialFunction)
		{
			UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Context.MaterialFunction, ExistingNode);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), NodeId);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDuplicateNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString NodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}

	int32 OffsetX = 40;
	int32 OffsetY = 40;
	double OffsetXDouble = 0.0;
	double OffsetYDouble = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("offset_x"), OffsetXDouble))
	{
		OffsetX = static_cast<int32>(OffsetXDouble);
	}
	if (Request.Params->TryGetNumberField(TEXT("offset_y"), OffsetYDouble))
	{
		OffsetY = static_cast<int32>(OffsetYDouble);
	}

	auto Task = [AssetPath, NodeId, OffsetX, OffsetY]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* ExistingNode = FindNodeById(Context, NodeId);
		if (!ExistingNode)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *NodeId));
		}

		UMaterialExpression* NewNode = nullptr;
		if (UMaterialExpressionComment* ExistingComment = Cast<UMaterialExpressionComment>(ExistingNode))
		{
			UMaterialExpressionComment* NewComment = DuplicateObject<UMaterialExpressionComment>(ExistingComment, Context.GetOuter());
			if (!NewComment)
			{
				return MakeFailure(TEXT("Failed to duplicate comment node"));
			}
			if (Context.Material)
			{
				NewComment->Material = Context.Material;
				Context.Material->GetExpressionCollection().AddComment(NewComment);
			}
			else if (Context.MaterialFunction)
			{
				Context.MaterialFunction->GetExpressionCollection().AddComment(NewComment);
			}
			NewComment->MaterialExpressionEditorX += OffsetX;
			NewComment->MaterialExpressionEditorY += OffsetY;
			NewComment->UpdateMaterialExpressionGuid(true, true);
			NewNode = NewComment;
		}
		else
		{
			NewNode = UMaterialEditingLibrary::DuplicateMaterialExpression(Context.Material, Context.MaterialFunction, ExistingNode);
			if (!NewNode)
			{
				return MakeFailure(TEXT("Failed to duplicate node"));
			}
			NewNode->MaterialExpressionEditorX += OffsetX;
			NewNode->MaterialExpressionEditorY += OffsetY;
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("source_node_id"), GetNodeId(ExistingNode));
		Result->SetObjectField(TEXT("node"), BuildNodeJson(NewNode));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMoveNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString NodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}

	double NodePosXDouble = 0.0;
	double NodePosYDouble = 0.0;
	if (!Request.Params->TryGetNumberField(TEXT("node_pos_x"), NodePosXDouble))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_pos_x'"));
	}
	if (!Request.Params->TryGetNumberField(TEXT("node_pos_y"), NodePosYDouble))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_pos_y'"));
	}
	const int32 NodePosX = static_cast<int32>(NodePosXDouble);
	const int32 NodePosY = static_cast<int32>(NodePosYDouble);

	auto Task = [AssetPath, NodeId, NodePosX, NodePosY]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* Node = FindNodeById(Context, NodeId);
		if (!Node)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *NodeId));
		}

		Node->Modify();
		Node->MaterialExpressionEditorX = NodePosX;
		Node->MaterialExpressionEditorY = NodePosY;
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(Node));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleAddCommentNode(const FMCPRequest& Request)
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

	FString Text = TEXT("Comment");
	Request.Params->TryGetStringField(TEXT("text"), Text);

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	int32 SizeX = 400;
	int32 SizeY = 100;
	double NumberField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NumberField)) NodePosX = static_cast<int32>(NumberField);
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NumberField)) NodePosY = static_cast<int32>(NumberField);
	if (Request.Params->TryGetNumberField(TEXT("size_x"), NumberField)) SizeX = static_cast<int32>(NumberField);
	if (Request.Params->TryGetNumberField(TEXT("size_y"), NumberField)) SizeY = static_cast<int32>(NumberField);

	FLinearColor CommentColor = FLinearColor::White;
	const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
	if (Request.Params->TryGetArrayField(TEXT("comment_color"), ColorArray) && ColorArray && ColorArray->Num() >= 3)
	{
		double R = 1.0;
		double G = 1.0;
		double B = 1.0;
		double A = 1.0;
		(*ColorArray)[0]->TryGetNumber(R);
		(*ColorArray)[1]->TryGetNumber(G);
		(*ColorArray)[2]->TryGetNumber(B);
		if (ColorArray->Num() > 3)
		{
			(*ColorArray)[3]->TryGetNumber(A);
		}
		CommentColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
	}

	auto Task = [AssetPath, Text, NodePosX, NodePosY, SizeX, SizeY, CommentColor]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpressionComment* NewComment = NewObject<UMaterialExpressionComment>(Context.GetOuter(), NAME_None, RF_Transactional);
		if (!NewComment)
		{
			return MakeFailure(TEXT("Failed to create comment node"));
		}

		if (Context.Material)
		{
			NewComment->Material = Context.Material;
			Context.Material->GetExpressionCollection().AddComment(NewComment);
		}
		else if (Context.MaterialFunction)
		{
			Context.MaterialFunction->GetExpressionCollection().AddComment(NewComment);
		}

		NewComment->MaterialExpressionEditorX = NodePosX;
		NewComment->MaterialExpressionEditorY = NodePosY;
		NewComment->SizeX = SizeX;
		NewComment->SizeY = SizeY;
		NewComment->Text = Text;
		NewComment->CommentColor = CommentColor;
		NewComment->UpdateMaterialExpressionGuid(true, true);
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(NewComment));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleAddRerouteNode(const FMCPRequest& Request)
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

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	double NumberField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NumberField)) NodePosX = static_cast<int32>(NumberField);
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NumberField)) NodePosY = static_cast<int32>(NumberField);

	auto Task = [AssetPath, NodePosX, NodePosY]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* NewNode = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Context.Material,
			Context.MaterialFunction,
			UMaterialExpressionReroute::StaticClass(),
			nullptr,
			NodePosX,
			NodePosY,
			true
		);
		if (!NewNode)
		{
			return MakeFailure(TEXT("Failed to create reroute node"));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(NewNode));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleLayoutGraph(const FMCPRequest& Request)
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

	auto Task = [AssetPath]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		if (Context.Material)
		{
			UMaterialEditingLibrary::LayoutMaterialExpressions(Context.Material);
		}
		else if (Context.MaterialFunction)
		{
			UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Context.MaterialFunction);
		}
		Context.MarkDirty();

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("asset_type"), Context.Material ? TEXT("material") : TEXT("material_function"));
		Result->SetNumberField(TEXT("expression_count"), Expressions.Num());
		Result->SetNumberField(TEXT("comment_count"), Comments.Num());
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
		PhasesObj->SetBoolField(TEXT("phase_2_graph_management"), true);
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
