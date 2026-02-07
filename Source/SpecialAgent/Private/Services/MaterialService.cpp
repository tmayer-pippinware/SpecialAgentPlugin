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

	static FString MaterialValueTypeToString(const uint64 TypeMask)
	{
		struct FTypeName
		{
			uint64 Mask;
			const TCHAR* Name;
		};

		static const FTypeName TypeNames[] =
		{
			{ static_cast<uint64>(MCT_Float1), TEXT("float1") },
			{ static_cast<uint64>(MCT_Float2), TEXT("float2") },
			{ static_cast<uint64>(MCT_Float3), TEXT("float3") },
			{ static_cast<uint64>(MCT_Float4), TEXT("float4") },
			{ static_cast<uint64>(MCT_Texture2D), TEXT("texture2d") },
			{ static_cast<uint64>(MCT_TextureCube), TEXT("texture_cube") },
			{ static_cast<uint64>(MCT_Texture2DArray), TEXT("texture2d_array") },
			{ static_cast<uint64>(MCT_TextureCubeArray), TEXT("texture_cube_array") },
			{ static_cast<uint64>(MCT_VolumeTexture), TEXT("volume_texture") },
			{ static_cast<uint64>(MCT_StaticBool), TEXT("static_bool") },
			{ static_cast<uint64>(MCT_Bool), TEXT("bool") },
			{ static_cast<uint64>(MCT_Unknown), TEXT("unknown") },
			{ static_cast<uint64>(MCT_MaterialAttributes), TEXT("material_attributes") },
			{ static_cast<uint64>(MCT_TextureExternal), TEXT("texture_external") },
			{ static_cast<uint64>(MCT_TextureVirtual), TEXT("texture_virtual") },
			{ static_cast<uint64>(MCT_SparseVolumeTexture), TEXT("sparse_volume_texture") },
			{ static_cast<uint64>(MCT_VTPageTableResult), TEXT("vt_page_table_result") },
			{ static_cast<uint64>(MCT_ShadingModel), TEXT("shading_model") },
			{ static_cast<uint64>(MCT_Substrate), TEXT("substrate") },
			{ static_cast<uint64>(MCT_LWCScalar), TEXT("lwc_scalar") },
			{ static_cast<uint64>(MCT_LWCVector2), TEXT("lwc_vector2") },
			{ static_cast<uint64>(MCT_LWCVector3), TEXT("lwc_vector3") },
			{ static_cast<uint64>(MCT_LWCVector4), TEXT("lwc_vector4") },
			{ static_cast<uint64>(MCT_Execution), TEXT("execution") },
			{ static_cast<uint64>(MCT_VoidStatement), TEXT("void_statement") },
			{ static_cast<uint64>(MCT_UInt1), TEXT("uint1") },
			{ static_cast<uint64>(MCT_UInt2), TEXT("uint2") },
			{ static_cast<uint64>(MCT_UInt3), TEXT("uint3") },
			{ static_cast<uint64>(MCT_UInt4), TEXT("uint4") },
			{ static_cast<uint64>(MCT_TextureCollection), TEXT("texture_collection") },
			{ static_cast<uint64>(MCT_TextureMeshPaint), TEXT("texture_mesh_paint") },
			{ static_cast<uint64>(MCT_TextureMaterialCache), TEXT("texture_material_cache") },
			{ static_cast<uint64>(MCT_Float3x3), TEXT("float3x3") },
			{ static_cast<uint64>(MCT_Float4x4), TEXT("float4x4") },
			{ static_cast<uint64>(MCT_LWCMatrix), TEXT("lwc_matrix") },
			{ static_cast<uint64>(MCT_MaterialCacheABuffer), TEXT("material_cache_abuffer") },
			{ static_cast<uint64>(MCT_Unexposed), TEXT("unexposed") },
		};

		if (TypeMask == 0)
		{
			return TEXT("none");
		}

		TArray<FString> SetFlags;
		for (const FTypeName& TypeName : TypeNames)
		{
			if ((TypeMask & TypeName.Mask) != 0)
			{
				SetFlags.Add(TypeName.Name);
			}
		}

		if (SetFlags.Num() == 0)
		{
			return FString::Printf(TEXT("0x%llX"), TypeMask);
		}

		return FString::Join(SetFlags, TEXT("|"));
	}

	static FString GetInputPinDisplayName(UMaterialExpression* Node, int32 InputIndex, const FExpressionInput* Input)
	{
		if (!Node)
		{
			return FString();
		}

		const FName InputName = Node->GetInputName(InputIndex);
		if (!InputName.IsNone())
		{
			return InputName.ToString();
		}

		if (Input && !Input->InputName.IsNone())
		{
			return Input->InputName.ToString();
		}

		return FString::Printf(TEXT("Input%d"), InputIndex);
	}

	static FString GetOutputPinDisplayName(UMaterialExpression* Node, int32 OutputIndex, const FExpressionOutput* Output)
	{
		if (Output && !Output->OutputName.IsNone())
		{
			return Output->OutputName.ToString();
		}

		return FString::Printf(TEXT("Output%d"), OutputIndex);
	}

	static bool TryResolveInputPinIndex(
		UMaterialExpression* Node,
		const FString& PinName,
		const bool bHasPinIndex,
		const int32 PinIndex,
		int32& OutPinIndex,
		FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Invalid node");
			return false;
		}

		if (bHasPinIndex)
		{
			if (PinIndex < 0 || !Node->GetInput(PinIndex))
			{
				OutError = FString::Printf(TEXT("Input pin index out of range: %d"), PinIndex);
				return false;
			}

			OutPinIndex = PinIndex;
			return true;
		}

		const FString TrimmedName = PinName.TrimStartAndEnd();
		if (!TrimmedName.IsEmpty())
		{
			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = Node->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}

				const FString DisplayName = GetInputPinDisplayName(Node, InputIndex, Input);
				if (DisplayName.Equals(TrimmedName, ESearchCase::IgnoreCase))
				{
					OutPinIndex = InputIndex;
					return true;
				}
			}

			OutError = FString::Printf(TEXT("Input pin not found: %s"), *TrimmedName);
			return false;
		}

		if (Node->GetInput(0))
		{
			OutPinIndex = 0;
			return true;
		}

		OutError = FString::Printf(TEXT("Node has no input pins: %s"), *Node->GetName());
		return false;
	}

	static bool TryResolveOutputPinIndex(
		UMaterialExpression* Node,
		const FString& PinName,
		const bool bHasPinIndex,
		const int32 PinIndex,
		int32& OutPinIndex,
		FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Invalid node");
			return false;
		}

		if (bHasPinIndex)
		{
			if (PinIndex < 0 || !Node->GetOutput(PinIndex))
			{
				OutError = FString::Printf(TEXT("Output pin index out of range: %d"), PinIndex);
				return false;
			}

			OutPinIndex = PinIndex;
			return true;
		}

		const FString TrimmedName = PinName.TrimStartAndEnd();
		if (!TrimmedName.IsEmpty())
		{
			for (int32 OutputIndex = 0;; ++OutputIndex)
			{
				FExpressionOutput* Output = Node->GetOutput(OutputIndex);
				if (!Output)
				{
					break;
				}

				const FString DisplayName = GetOutputPinDisplayName(Node, OutputIndex, Output);
				if (DisplayName.Equals(TrimmedName, ESearchCase::IgnoreCase))
				{
					OutPinIndex = OutputIndex;
					return true;
				}
			}

			OutError = FString::Printf(TEXT("Output pin not found: %s"), *TrimmedName);
			return false;
		}

		if (Node->GetOutput(0))
		{
			OutPinIndex = 0;
			return true;
		}

		OutError = FString::Printf(TEXT("Node has no output pins: %s"), *Node->GetName());
		return false;
	}

	static bool BreakExpressionInputLink(FExpressionInput* Input)
	{
		if (!Input || !Input->Expression)
		{
			return false;
		}

		Input->Expression = nullptr;
		Input->OutputIndex = 0;
		Input->SetMask(0, 0, 0, 0, 0);
		return true;
	}

	static int32 CountOutputPinLinks(const FMaterialGraphContext& Context, UMaterialExpression* Node, int32 OutputIndex)
	{
		if (!Node)
		{
			return 0;
		}

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		int32 LinkCount = 0;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression)
			{
				continue;
			}

			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = Expression->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}

				if (Input->Expression == Node && Input->OutputIndex == OutputIndex)
				{
					++LinkCount;
				}
			}
		}

		return LinkCount;
	}

	static TSharedPtr<FJsonObject> BuildInputPinJson(UMaterialExpression* Node, int32 InputIndex, FExpressionInput* Input, bool bIncludeDefaultValue)
	{
		TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
		Pin->SetStringField(TEXT("pin_direction"), TEXT("input"));
		Pin->SetNumberField(TEXT("pin_index"), InputIndex);
		Pin->SetStringField(TEXT("pin_name"), GetInputPinDisplayName(Node, InputIndex, Input));
		Pin->SetNumberField(TEXT("value_type_mask"), static_cast<double>(Node ? static_cast<uint64>(Node->GetInputValueType(InputIndex)) : 0.0));
		Pin->SetStringField(TEXT("value_type"), MaterialValueTypeToString(Node ? static_cast<uint64>(Node->GetInputValueType(InputIndex)) : 0));

		const bool bConnected = Input && Input->Expression != nullptr;
		Pin->SetBoolField(TEXT("connected"), bConnected);
		if (bConnected)
		{
			Pin->SetStringField(TEXT("linked_node_id"), GetNodeId(Input->Expression));
			Pin->SetStringField(TEXT("linked_node_name"), Input->Expression->GetName());
			Pin->SetNumberField(TEXT("linked_output_index"), Input->OutputIndex);
			Pin->SetStringField(TEXT("linked_output_name"), GetOutputPinDisplayName(Input->Expression, Input->OutputIndex, Input->Expression->GetOutput(Input->OutputIndex)));
		}

		Pin->SetBoolField(TEXT("mask_enabled"), Input ? Input->Mask != 0 : false);
		Pin->SetNumberField(TEXT("mask"), Input ? Input->Mask : 0);
		Pin->SetNumberField(TEXT("mask_r"), Input ? Input->MaskR : 0);
		Pin->SetNumberField(TEXT("mask_g"), Input ? Input->MaskG : 0);
		Pin->SetNumberField(TEXT("mask_b"), Input ? Input->MaskB : 0);
		Pin->SetNumberField(TEXT("mask_a"), Input ? Input->MaskA : 0);

		if (bIncludeDefaultValue)
		{
			Pin->SetStringField(TEXT("default_value"), Node ? Node->GetInputPinDefaultValue(InputIndex) : FString());
		}

		return Pin;
	}

	static TSharedPtr<FJsonObject> BuildOutputPinJson(const FMaterialGraphContext& Context, UMaterialExpression* Node, int32 OutputIndex, FExpressionOutput* Output)
	{
		TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
		Pin->SetStringField(TEXT("pin_direction"), TEXT("output"));
		Pin->SetNumberField(TEXT("pin_index"), OutputIndex);
		Pin->SetStringField(TEXT("pin_name"), GetOutputPinDisplayName(Node, OutputIndex, Output));
		Pin->SetNumberField(TEXT("value_type_mask"), static_cast<double>(Node ? static_cast<uint64>(Node->GetOutputValueType(OutputIndex)) : 0.0));
		Pin->SetStringField(TEXT("value_type"), MaterialValueTypeToString(Node ? static_cast<uint64>(Node->GetOutputValueType(OutputIndex)) : 0));
		Pin->SetNumberField(TEXT("connected_link_count"), CountOutputPinLinks(Context, Node, OutputIndex));

		Pin->SetBoolField(TEXT("mask_enabled"), Output ? Output->Mask != 0 : false);
		Pin->SetNumberField(TEXT("mask"), Output ? Output->Mask : 0);
		Pin->SetNumberField(TEXT("mask_r"), Output ? Output->MaskR : 0);
		Pin->SetNumberField(TEXT("mask_g"), Output ? Output->MaskG : 0);
		Pin->SetNumberField(TEXT("mask_b"), Output ? Output->MaskB : 0);
		Pin->SetNumberField(TEXT("mask_a"), Output ? Output->MaskA : 0);

		return Pin;
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

	struct FMaterialOutputAlias
	{
		const TCHAR* Name;
		EMaterialProperty Property;
	};

	static const FMaterialOutputAlias GMaterialOutputAliases[] =
	{
		{ TEXT("emissive"), MP_EmissiveColor },
		{ TEXT("emissive_color"), MP_EmissiveColor },
		{ TEXT("opacity"), MP_Opacity },
		{ TEXT("opacity_mask"), MP_OpacityMask },
		{ TEXT("base_color"), MP_BaseColor },
		{ TEXT("basecolor"), MP_BaseColor },
		{ TEXT("diffuse"), MP_BaseColor },
		{ TEXT("metallic"), MP_Metallic },
		{ TEXT("specular"), MP_Specular },
		{ TEXT("roughness"), MP_Roughness },
		{ TEXT("anisotropy"), MP_Anisotropy },
		{ TEXT("normal"), MP_Normal },
		{ TEXT("tangent"), MP_Tangent },
		{ TEXT("world_position_offset"), MP_WorldPositionOffset },
		{ TEXT("worldpositionoffset"), MP_WorldPositionOffset },
		{ TEXT("wpo"), MP_WorldPositionOffset },
		{ TEXT("subsurface"), MP_SubsurfaceColor },
		{ TEXT("subsurface_color"), MP_SubsurfaceColor },
		{ TEXT("ambient_occlusion"), MP_AmbientOcclusion },
		{ TEXT("ao"), MP_AmbientOcclusion },
		{ TEXT("refraction"), MP_Refraction },
		{ TEXT("front_material"), MP_FrontMaterial },
		{ TEXT("surface_thickness"), MP_SurfaceThickness },
		{ TEXT("displacement"), MP_Displacement },
		{ TEXT("material_attributes"), MP_MaterialAttributes },
		{ TEXT("shading_model"), MP_ShadingModel },
		{ TEXT("pixel_depth_offset"), MP_PixelDepthOffset },
		{ TEXT("pdo"), MP_PixelDepthOffset },
	};

	static bool IsCustomUVProperty(const EMaterialProperty Property)
	{
		const int32 PropertyIndex = static_cast<int32>(Property);
		const int32 FirstCustomUVPropertyIndex = static_cast<int32>(MP_CustomizedUVs0);
		return PropertyIndex >= FirstCustomUVPropertyIndex && PropertyIndex < FirstCustomUVPropertyIndex + 8;
	}

	static bool TryGetCustomUVIndex(const EMaterialProperty Property, int32& OutCustomUVIndex)
	{
		if (!IsCustomUVProperty(Property))
		{
			OutCustomUVIndex = INDEX_NONE;
			return false;
		}

		const int32 FirstCustomUVPropertyIndex = static_cast<int32>(MP_CustomizedUVs0);
		OutCustomUVIndex = static_cast<int32>(Property) - FirstCustomUVPropertyIndex;
		return true;
	}

	static FString MaterialPropertyToOutputName(const EMaterialProperty Property)
	{
		int32 CustomUVIndex = INDEX_NONE;
		if (TryGetCustomUVIndex(Property, CustomUVIndex))
		{
			return FString::Printf(TEXT("custom_uv_%d"), CustomUVIndex);
		}

		switch (Property)
		{
		case MP_EmissiveColor: return TEXT("emissive_color");
		case MP_Opacity: return TEXT("opacity");
		case MP_OpacityMask: return TEXT("opacity_mask");
		case MP_BaseColor: return TEXT("base_color");
		case MP_Metallic: return TEXT("metallic");
		case MP_Specular: return TEXT("specular");
		case MP_Roughness: return TEXT("roughness");
		case MP_Anisotropy: return TEXT("anisotropy");
		case MP_Normal: return TEXT("normal");
		case MP_Tangent: return TEXT("tangent");
		case MP_WorldPositionOffset: return TEXT("world_position_offset");
		case MP_SubsurfaceColor: return TEXT("subsurface_color");
		case MP_AmbientOcclusion: return TEXT("ambient_occlusion");
		case MP_Refraction: return TEXT("refraction");
		case MP_PixelDepthOffset: return TEXT("pixel_depth_offset");
		case MP_ShadingModel: return TEXT("shading_model");
		case MP_FrontMaterial: return TEXT("front_material");
		case MP_SurfaceThickness: return TEXT("surface_thickness");
		case MP_Displacement: return TEXT("displacement");
		case MP_MaterialAttributes: return TEXT("material_attributes");
		default: return TEXT("unknown");
		}
	}

	static void AddPhase4MaterialOutputProperties(TArray<EMaterialProperty>& OutProperties)
	{
		OutProperties.Add(MP_BaseColor);
		OutProperties.Add(MP_Metallic);
		OutProperties.Add(MP_Specular);
		OutProperties.Add(MP_Roughness);
		OutProperties.Add(MP_Anisotropy);
		OutProperties.Add(MP_Normal);
		OutProperties.Add(MP_Tangent);
		OutProperties.Add(MP_EmissiveColor);
		OutProperties.Add(MP_Opacity);
		OutProperties.Add(MP_OpacityMask);
		OutProperties.Add(MP_WorldPositionOffset);
		OutProperties.Add(MP_SubsurfaceColor);
		OutProperties.Add(MP_AmbientOcclusion);
		OutProperties.Add(MP_Refraction);
		OutProperties.Add(MP_PixelDepthOffset);
		OutProperties.Add(MP_ShadingModel);
		OutProperties.Add(MP_FrontMaterial);
		OutProperties.Add(MP_SurfaceThickness);
		OutProperties.Add(MP_Displacement);
		OutProperties.Add(MP_MaterialAttributes);
		for (int32 CustomUVIndex = 0; CustomUVIndex < 8; ++CustomUVIndex)
		{
			OutProperties.Add(static_cast<EMaterialProperty>(static_cast<int32>(MP_CustomizedUVs0) + CustomUVIndex));
		}
	}

	static bool ParseMaterialOutputProperty(
		const FString& OutputNameInput,
		const bool bHasCustomUVIndex,
		const int32 CustomUVIndex,
		EMaterialProperty& OutProperty,
		FString& OutResolvedOutputName,
		FString& OutError)
	{
		FString Normalized = OutputNameInput.TrimStartAndEnd().ToLower();
		Normalized = Normalized.Replace(TEXT(" "), TEXT("_"));
		Normalized = Normalized.Replace(TEXT("-"), TEXT("_"));

		if (Normalized.IsEmpty())
		{
			OutError = TEXT("Missing output property name");
			return false;
		}

		if (Normalized.StartsWith(TEXT("custom_uv")))
		{
			int32 ParsedCustomUVIndex = INDEX_NONE;
			if (bHasCustomUVIndex)
			{
				ParsedCustomUVIndex = CustomUVIndex;
			}
			else
			{
				FString Suffix = Normalized.RightChop(9); // "custom_uv"
				if (Suffix.StartsWith(TEXT("_")))
				{
					Suffix = Suffix.RightChop(1);
				}
				if (!Suffix.IsEmpty() && FDefaultValueHelper::ParseInt(Suffix, ParsedCustomUVIndex))
				{
					// Parsed from output_name
				}
			}

			if (ParsedCustomUVIndex < 0 || ParsedCustomUVIndex > 7)
			{
				OutError = TEXT("Custom UV output requires uv_index in range [0, 7] (or output_name like custom_uv_0)");
				return false;
			}

			OutProperty = static_cast<EMaterialProperty>(static_cast<int32>(MP_CustomizedUVs0) + ParsedCustomUVIndex);
			OutResolvedOutputName = FString::Printf(TEXT("custom_uv_%d"), ParsedCustomUVIndex);
			return true;
		}

		for (const FMaterialOutputAlias& Alias : GMaterialOutputAliases)
		{
			if (Normalized.Equals(Alias.Name, ESearchCase::CaseSensitive))
			{
				OutProperty = Alias.Property;
				OutResolvedOutputName = MaterialPropertyToOutputName(Alias.Property);
				return true;
			}
		}

		int32 NumericProperty = INDEX_NONE;
		if (FDefaultValueHelper::ParseInt(Normalized, NumericProperty) &&
			NumericProperty >= 0 &&
			NumericProperty < static_cast<int32>(MP_MAX))
		{
			OutProperty = static_cast<EMaterialProperty>(NumericProperty);
			OutResolvedOutputName = MaterialPropertyToOutputName(OutProperty);
			return true;
		}

		OutError = FString::Printf(TEXT("Unknown material output property: %s"), *OutputNameInput);
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
	AddTool(TEXT("list_node_pins"), TEXT("List pins for a graph node."));
	AddTool(TEXT("connect_pins"), TEXT("Connect a node output pin to another node input pin."));
	AddTool(TEXT("disconnect_pins"), TEXT("Disconnect a link between two nodes."));
	AddTool(TEXT("break_pin_links"), TEXT("Break all links for a specific pin."));
	AddTool(TEXT("break_all_node_links"), TEXT("Break all incoming and outgoing links for a node."));
	AddTool(TEXT("set_pin_default_value"), TEXT("Set the default value for an input pin."));
	AddTool(TEXT("reset_pin_default_value"), TEXT("Reset an input pin default value to class defaults."));
	AddTool(TEXT("set_material_output"), TEXT("Connect a node output to a material output property."));
	AddTool(TEXT("clear_material_output"), TEXT("Clear a material output property connection."));
	AddTool(TEXT("list_connected_outputs"), TEXT("List material output properties and current graph connections."));
	AddTool(TEXT("set_custom_uv_output"), TEXT("Connect a node output to a custom UV channel."));
	AddTool(TEXT("set_pixel_depth_offset_output"), TEXT("Connect a node output to the pixel depth offset output."));
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
	if (MethodName == TEXT("list_node_pins")) return HandleListNodePins(Request);
	if (MethodName == TEXT("connect_pins")) return HandleConnectPins(Request);
	if (MethodName == TEXT("disconnect_pins")) return HandleDisconnectPins(Request);
	if (MethodName == TEXT("break_pin_links")) return HandleBreakPinLinks(Request);
	if (MethodName == TEXT("break_all_node_links")) return HandleBreakAllNodeLinks(Request);
	if (MethodName == TEXT("set_pin_default_value")) return HandleSetPinDefaultValue(Request);
	if (MethodName == TEXT("reset_pin_default_value")) return HandleResetPinDefaultValue(Request);
	if (MethodName == TEXT("set_material_output")) return HandleSetMaterialOutput(Request);
	if (MethodName == TEXT("clear_material_output")) return HandleClearMaterialOutput(Request);
	if (MethodName == TEXT("list_connected_outputs")) return HandleListConnectedOutputs(Request);
	if (MethodName == TEXT("set_custom_uv_output")) return HandleSetCustomUVOutput(Request);
	if (MethodName == TEXT("set_pixel_depth_offset_output")) return HandleSetPixelDepthOffsetOutput(Request);
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

FMCPResponse FMaterialService::HandleListNodePins(const FMCPRequest& Request)
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

	bool bIncludeDefaultValues = true;
	Request.Params->TryGetBoolField(TEXT("include_default_values"), bIncludeDefaultValues);

	auto Task = [AssetPath, NodeId, bIncludeDefaultValues]() -> TSharedPtr<FJsonObject>
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
		if (Node->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes do not have material pins"));
		}

		TArray<TSharedPtr<FJsonValue>> InputPins;
		for (int32 InputIndex = 0;; ++InputIndex)
		{
			FExpressionInput* Input = Node->GetInput(InputIndex);
			if (!Input)
			{
				break;
			}
			InputPins.Add(MakeShared<FJsonValueObject>(BuildInputPinJson(Node, InputIndex, Input, bIncludeDefaultValues)));
		}

		TArray<TSharedPtr<FJsonValue>> OutputPins;
		for (int32 OutputIndex = 0;; ++OutputIndex)
		{
			FExpressionOutput* Output = Node->GetOutput(OutputIndex);
			if (!Output)
			{
				break;
			}
			OutputPins.Add(MakeShared<FJsonValueObject>(BuildOutputPinJson(Context, Node, OutputIndex, Output)));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
		Result->SetStringField(TEXT("node_name"), Node->GetName());
		Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		Result->SetArrayField(TEXT("input_pins"), InputPins);
		Result->SetArrayField(TEXT("output_pins"), OutputPins);
		Result->SetNumberField(TEXT("input_pin_count"), InputPins.Num());
		Result->SetNumberField(TEXT("output_pin_count"), OutputPins.Num());
		Result->SetBoolField(TEXT("include_default_values"), bIncludeDefaultValues);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleConnectPins(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString FromNodeId;
	FString ToNodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("to_node_id"), ToNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'to_node_id'"));
	}

	FString FromOutputPin;
	FString ToInputPin;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPin);
	Request.Params->TryGetStringField(TEXT("to_input_pin"), ToInputPin);

	bool bHasFromOutputIndex = false;
	bool bHasToInputIndex = false;
	int32 FromOutputIndex = 0;
	int32 ToInputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericField))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericField);
	}
	if (Request.Params->TryGetNumberField(TEXT("to_input_index"), NumericField))
	{
		bHasToInputIndex = true;
		ToInputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, FromNodeId, ToNodeId, FromOutputPin, ToInputPin, bHasFromOutputIndex, FromOutputIndex, bHasToInputIndex, ToInputIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* FromNode = FindNodeById(Context, FromNodeId);
		if (!FromNode)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
		}
		UMaterialExpression* ToNode = FindNodeById(Context, ToNodeId);
		if (!ToNode)
		{
			return MakeFailure(FString::Printf(TEXT("Target node not found: %s"), *ToNodeId));
		}
		if (FromNode->IsA<UMaterialExpressionComment>() || ToNode->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Cannot connect comment nodes"));
		}

		int32 ResolvedFromOutputIndex = INDEX_NONE;
		if (!TryResolveOutputPinIndex(FromNode, FromOutputPin, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
		{
			return MakeFailure(Error);
		}

		int32 ResolvedToInputIndex = INDEX_NONE;
		if (!TryResolveInputPinIndex(ToNode, ToInputPin, bHasToInputIndex, ToInputIndex, ResolvedToInputIndex, Error))
		{
			return MakeFailure(Error);
		}

		FExpressionInput* Input = ToNode->GetInput(ResolvedToInputIndex);
		if (!Input)
		{
			return MakeFailure(FString::Printf(TEXT("Target input pin index out of range: %d"), ResolvedToInputIndex));
		}

		FromNode->Modify();
		ToNode->Modify();
		FromNode->ConnectExpression(Input, ResolvedFromOutputIndex);
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("from_node_id"), GetNodeId(FromNode));
		Result->SetStringField(TEXT("to_node_id"), GetNodeId(ToNode));
		Result->SetNumberField(TEXT("from_output_index"), ResolvedFromOutputIndex);
		Result->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(FromNode, ResolvedFromOutputIndex, FromNode->GetOutput(ResolvedFromOutputIndex)));
		Result->SetNumberField(TEXT("to_input_index"), ResolvedToInputIndex);
		Result->SetStringField(TEXT("to_input_name"), GetInputPinDisplayName(ToNode, ResolvedToInputIndex, Input));
		Result->SetObjectField(TEXT("to_input_pin"), BuildInputPinJson(ToNode, ResolvedToInputIndex, Input, true));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDisconnectPins(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString FromNodeId;
	FString ToNodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("to_node_id"), ToNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'to_node_id'"));
	}

	FString FromOutputPin;
	FString ToInputPin;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPin);
	Request.Params->TryGetStringField(TEXT("to_input_pin"), ToInputPin);

	bool bHasFromOutputIndex = false;
	bool bHasToInputIndex = false;
	int32 FromOutputIndex = 0;
	int32 ToInputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericField))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericField);
	}
	if (Request.Params->TryGetNumberField(TEXT("to_input_index"), NumericField))
	{
		bHasToInputIndex = true;
		ToInputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, FromNodeId, ToNodeId, FromOutputPin, ToInputPin, bHasFromOutputIndex, FromOutputIndex, bHasToInputIndex, ToInputIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* FromNode = FindNodeById(Context, FromNodeId);
		if (!FromNode)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
		}
		UMaterialExpression* ToNode = FindNodeById(Context, ToNodeId);
		if (!ToNode)
		{
			return MakeFailure(FString::Printf(TEXT("Target node not found: %s"), *ToNodeId));
		}
		if (FromNode->IsA<UMaterialExpressionComment>() || ToNode->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Cannot disconnect comment nodes"));
		}

		const bool bHasFromSelector = bHasFromOutputIndex || !FromOutputPin.TrimStartAndEnd().IsEmpty();
		const bool bHasToSelector = bHasToInputIndex || !ToInputPin.TrimStartAndEnd().IsEmpty();

		int32 ResolvedFromOutputIndex = INDEX_NONE;
		if (bHasFromSelector)
		{
			if (!TryResolveOutputPinIndex(FromNode, FromOutputPin, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
			{
				return MakeFailure(Error);
			}
		}

		int32 ResolvedToInputIndex = INDEX_NONE;
		if (bHasToSelector)
		{
			if (!TryResolveInputPinIndex(ToNode, ToInputPin, bHasToInputIndex, ToInputIndex, ResolvedToInputIndex, Error))
			{
				return MakeFailure(Error);
			}
		}

		int32 DisconnectedLinks = 0;
		ToNode->Modify();

		auto TryDisconnectInputByIndex = [&](int32 InputIndex)
		{
			FExpressionInput* Input = ToNode->GetInput(InputIndex);
			if (!Input || Input->Expression != FromNode)
			{
				return;
			}
			if (bHasFromSelector && Input->OutputIndex != ResolvedFromOutputIndex)
			{
				return;
			}
			if (BreakExpressionInputLink(Input))
			{
				++DisconnectedLinks;
			}
		};

		if (bHasToSelector)
		{
			TryDisconnectInputByIndex(ResolvedToInputIndex);
		}
		else
		{
			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = ToNode->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}
				TryDisconnectInputByIndex(InputIndex);
			}
		}

		if (DisconnectedLinks > 0)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("from_node_id"), GetNodeId(FromNode));
		Result->SetStringField(TEXT("to_node_id"), GetNodeId(ToNode));
		Result->SetNumberField(TEXT("disconnected_links"), DisconnectedLinks);
		Result->SetBoolField(TEXT("changed"), DisconnectedLinks > 0);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleBreakPinLinks(const FMCPRequest& Request)
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

	FString PinDirection;
	Request.Params->TryGetStringField(TEXT("pin_direction"), PinDirection);

	FString PinName;
	Request.Params->TryGetStringField(TEXT("pin_name"), PinName);

	bool bHasPinIndex = false;
	int32 PinIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("pin_index"), NumericField))
	{
		bHasPinIndex = true;
		PinIndex = static_cast<int32>(NumericField);
	}

	FString InputPinName;
	FString OutputPinName;
	Request.Params->TryGetStringField(TEXT("input_pin"), InputPinName);
	Request.Params->TryGetStringField(TEXT("output_pin"), OutputPinName);

	bool bHasInputIndex = false;
	bool bHasOutputIndex = false;
	int32 InputIndex = 0;
	int32 OutputIndex = 0;
	if (Request.Params->TryGetNumberField(TEXT("input_index"), NumericField))
	{
		bHasInputIndex = true;
		InputIndex = static_cast<int32>(NumericField);
	}
	if (Request.Params->TryGetNumberField(TEXT("output_index"), NumericField))
	{
		bHasOutputIndex = true;
		OutputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, NodeId, PinDirection, PinName, bHasPinIndex, PinIndex, InputPinName, OutputPinName, bHasInputIndex, InputIndex, bHasOutputIndex, OutputIndex]() -> TSharedPtr<FJsonObject>
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
		if (Node->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes do not have material pins"));
		}

		FString Direction = PinDirection.TrimStartAndEnd();
		if (Direction.IsEmpty())
		{
			if (!InputPinName.TrimStartAndEnd().IsEmpty() || bHasInputIndex)
			{
				Direction = TEXT("input");
			}
			else if (!OutputPinName.TrimStartAndEnd().IsEmpty() || bHasOutputIndex)
			{
				Direction = TEXT("output");
			}
			else
			{
				Direction = TEXT("input");
			}
		}

		int32 BrokenLinks = 0;
		int32 ResolvedPinIndex = INDEX_NONE;
		Node->Modify();

		if (Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
		{
			const FString EffectivePinName = !InputPinName.TrimStartAndEnd().IsEmpty() ? InputPinName : PinName;
			const bool bEffectiveHasPinIndex = bHasInputIndex || bHasPinIndex;
			const int32 EffectivePinIndex = bHasInputIndex ? InputIndex : PinIndex;
			if (!TryResolveInputPinIndex(Node, EffectivePinName, bEffectiveHasPinIndex, EffectivePinIndex, ResolvedPinIndex, Error))
			{
				return MakeFailure(Error);
			}

			FExpressionInput* Input = Node->GetInput(ResolvedPinIndex);
			if (BreakExpressionInputLink(Input))
			{
				BrokenLinks = 1;
			}
		}
		else if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
		{
			const FString EffectivePinName = !OutputPinName.TrimStartAndEnd().IsEmpty() ? OutputPinName : PinName;
			const bool bEffectiveHasPinIndex = bHasOutputIndex || bHasPinIndex;
			const int32 EffectivePinIndex = bHasOutputIndex ? OutputIndex : PinIndex;
			if (!TryResolveOutputPinIndex(Node, EffectivePinName, bEffectiveHasPinIndex, EffectivePinIndex, ResolvedPinIndex, Error))
			{
				return MakeFailure(Error);
			}

			TArray<UMaterialExpression*> Expressions;
			TArray<UMaterialExpressionComment*> Comments;
			GatherGraphNodes(Context, Expressions, Comments);

			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression)
				{
					continue;
				}

				for (int32 InputPinIndex = 0;; ++InputPinIndex)
				{
					FExpressionInput* Input = Expression->GetInput(InputPinIndex);
					if (!Input)
					{
						break;
					}

					if (Input->Expression == Node && Input->OutputIndex == ResolvedPinIndex)
					{
						Expression->Modify();
						if (BreakExpressionInputLink(Input))
						{
							++BrokenLinks;
						}
					}
				}
			}
		}
		else
		{
			return MakeFailure(FString::Printf(TEXT("Invalid pin_direction '%s'. Expected 'input' or 'output'."), *Direction));
		}

		if (BrokenLinks > 0)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
		Result->SetStringField(TEXT("pin_direction"), Direction.ToLower());
		Result->SetNumberField(TEXT("pin_index"), ResolvedPinIndex);
		Result->SetNumberField(TEXT("broken_links"), BrokenLinks);
		Result->SetBoolField(TEXT("changed"), BrokenLinks > 0);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleBreakAllNodeLinks(const FMCPRequest& Request)
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

		UMaterialExpression* Node = FindNodeById(Context, NodeId);
		if (!Node)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *NodeId));
		}
		if (Node->IsA<UMaterialExpressionComment>())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
			Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
			Result->SetNumberField(TEXT("broken_links"), 0);
			Result->SetBoolField(TEXT("changed"), false);
			return Result;
		}

		int32 BrokenLinks = 0;
		Node->Modify();

		for (int32 InputIndex = 0;; ++InputIndex)
		{
			FExpressionInput* Input = Node->GetInput(InputIndex);
			if (!Input)
			{
				break;
			}
			if (BreakExpressionInputLink(Input))
			{
				++BrokenLinks;
			}
		}

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression)
			{
				continue;
			}

			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = Expression->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}
				if (Input->Expression == Node)
				{
					Expression->Modify();
					if (BreakExpressionInputLink(Input))
					{
						++BrokenLinks;
					}
				}
			}
		}

		if (BrokenLinks > 0)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
		Result->SetNumberField(TEXT("broken_links"), BrokenLinks);
		Result->SetBoolField(TEXT("changed"), BrokenLinks > 0);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetPinDefaultValue(const FMCPRequest& Request)
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

	FString InputPinName;
	Request.Params->TryGetStringField(TEXT("input_pin"), InputPinName);

	bool bHasInputIndex = false;
	int32 InputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("input_index"), NumericField))
	{
		bHasInputIndex = true;
		InputIndex = static_cast<int32>(NumericField);
	}

	FString DefaultValue;
	if (!Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue))
	{
		double DefaultNumericValue = 0.0;
		bool bDefaultBoolValue = false;
		if (Request.Params->TryGetNumberField(TEXT("default_value"), DefaultNumericValue))
		{
			DefaultValue = FString::SanitizeFloat(DefaultNumericValue);
		}
		else if (Request.Params->TryGetBoolField(TEXT("default_value"), bDefaultBoolValue))
		{
			DefaultValue = bDefaultBoolValue ? TEXT("true") : TEXT("false");
		}
		else
		{
			return InvalidParams(Request.Id, TEXT("Missing required parameter 'default_value'"));
		}
	}

	auto Task = [AssetPath, NodeId, InputPinName, bHasInputIndex, InputIndex, DefaultValue]() -> TSharedPtr<FJsonObject>
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
		if (Node->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes do not have input pins"));
		}

		int32 ResolvedInputIndex = INDEX_NONE;
		if (!TryResolveInputPinIndex(Node, InputPinName, bHasInputIndex, InputIndex, ResolvedInputIndex, Error))
		{
			return MakeFailure(Error);
		}

		FExpressionInput* Input = Node->GetInput(ResolvedInputIndex);
		if (!Input)
		{
			return MakeFailure(FString::Printf(TEXT("Input pin index out of range: %d"), ResolvedInputIndex));
		}

		Node->Modify();
		bool bApplied = false;
		for (FProperty* Property : Node->GetInputPinProperty(ResolvedInputIndex))
		{
			if (!Property)
			{
				continue;
			}

			void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
			if (PropertyValuePtr && Property->ImportText_Direct(*DefaultValue, PropertyValuePtr, Node, PPF_None))
			{
				bApplied = true;
			}
		}

		if (!bApplied)
		{
			return MakeFailure(FString::Printf(TEXT("Could not apply default value '%s' to input pin '%s'"), *DefaultValue, *GetInputPinDisplayName(Node, ResolvedInputIndex, Input)));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
		Result->SetNumberField(TEXT("input_index"), ResolvedInputIndex);
		Result->SetStringField(TEXT("input_name"), GetInputPinDisplayName(Node, ResolvedInputIndex, Input));
		Result->SetStringField(TEXT("requested_default_value"), DefaultValue);
		Result->SetStringField(TEXT("applied_default_value"), Node->GetInputPinDefaultValue(ResolvedInputIndex));
		Result->SetObjectField(TEXT("input_pin"), BuildInputPinJson(Node, ResolvedInputIndex, Input, true));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleResetPinDefaultValue(const FMCPRequest& Request)
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

	FString InputPinName;
	Request.Params->TryGetStringField(TEXT("input_pin"), InputPinName);

	bool bHasInputIndex = false;
	int32 InputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("input_index"), NumericField))
	{
		bHasInputIndex = true;
		InputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, NodeId, InputPinName, bHasInputIndex, InputIndex]() -> TSharedPtr<FJsonObject>
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
		if (Node->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes do not have input pins"));
		}

		int32 ResolvedInputIndex = INDEX_NONE;
		if (!TryResolveInputPinIndex(Node, InputPinName, bHasInputIndex, InputIndex, ResolvedInputIndex, Error))
		{
			return MakeFailure(Error);
		}

		FExpressionInput* Input = Node->GetInput(ResolvedInputIndex);
		if (!Input)
		{
			return MakeFailure(FString::Printf(TEXT("Input pin index out of range: %d"), ResolvedInputIndex));
		}

		UMaterialExpression* ClassDefaultObject = Node->GetClass()->GetDefaultObject<UMaterialExpression>();
		if (!ClassDefaultObject)
		{
			return MakeFailure(TEXT("Failed to resolve class default object"));
		}

		const FString DefaultValue = ClassDefaultObject->GetInputPinDefaultValue(ResolvedInputIndex);

		Node->Modify();
		bool bCopiedProperties = false;
		for (FProperty* Property : Node->GetInputPinProperty(ResolvedInputIndex))
		{
			if (!Property)
			{
				continue;
			}

			Property->CopyCompleteValue_InContainer(Node, ClassDefaultObject);
			bCopiedProperties = true;
		}

		if (!bCopiedProperties)
		{
			return MakeFailure(FString::Printf(TEXT("Input pin does not expose a resettable default value: %s"), *GetInputPinDisplayName(Node, ResolvedInputIndex, Input)));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("node_id"), GetNodeId(Node));
		Result->SetNumberField(TEXT("input_index"), ResolvedInputIndex);
		Result->SetStringField(TEXT("input_name"), GetInputPinDisplayName(Node, ResolvedInputIndex, Input));
		Result->SetStringField(TEXT("reset_default_value"), Node->GetInputPinDefaultValue(ResolvedInputIndex));
		Result->SetObjectField(TEXT("input_pin"), BuildInputPinJson(Node, ResolvedInputIndex, Input, true));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetMaterialOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString OutputName;
	FString FromNodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("output_name"), OutputName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'output_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}

	FString FromOutputPinName;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPinName);

	bool bHasFromOutputIndex = false;
	int32 FromOutputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericField))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericField);
	}

	bool bHasCustomUVIndex = false;
	int32 CustomUVIndex = 0;
	if (Request.Params->TryGetNumberField(TEXT("uv_index"), NumericField))
	{
		bHasCustomUVIndex = true;
		CustomUVIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, OutputName, FromNodeId, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex, bHasCustomUVIndex, CustomUVIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_material_output only supports UMaterial assets"));
		}

		EMaterialProperty OutputProperty = MP_MAX;
		FString ResolvedOutputName;
		if (!ParseMaterialOutputProperty(OutputName, bHasCustomUVIndex, CustomUVIndex, OutputProperty, ResolvedOutputName, Error))
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* FromNode = FindNodeById(Context, FromNodeId);
		if (!FromNode)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
		}
		if (FromNode->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes cannot drive material outputs"));
		}

		int32 ResolvedFromOutputIndex = INDEX_NONE;
		if (!TryResolveOutputPinIndex(FromNode, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
		{
			return MakeFailure(Error);
		}

		FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
		if (!PropertyInput)
		{
			return MakeFailure(FString::Printf(TEXT("Material output is unavailable for this property: %s"), *ResolvedOutputName));
		}

		FromNode->Modify();
		Context.Material->Modify();
		FromNode->ConnectExpression(PropertyInput, ResolvedFromOutputIndex);
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("output_name"), ResolvedOutputName);
		Result->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
		Result->SetBoolField(TEXT("output_supported"), Context.Material->IsPropertySupported(OutputProperty));
		Result->SetBoolField(TEXT("output_active_in_editor"), Context.Material->IsPropertyActiveInEditor(OutputProperty));
		Result->SetStringField(TEXT("from_node_id"), GetNodeId(FromNode));
		Result->SetStringField(TEXT("from_node_name"), FromNode->GetName());
		Result->SetNumberField(TEXT("from_output_index"), ResolvedFromOutputIndex);
		Result->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(FromNode, ResolvedFromOutputIndex, FromNode->GetOutput(ResolvedFromOutputIndex)));

		int32 ResolvedCustomUVIndex = INDEX_NONE;
		if (TryGetCustomUVIndex(OutputProperty, ResolvedCustomUVIndex))
		{
			Result->SetNumberField(TEXT("uv_index"), ResolvedCustomUVIndex);
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleClearMaterialOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString OutputName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("output_name"), OutputName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'output_name'"));
	}

	bool bHasCustomUVIndex = false;
	int32 CustomUVIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("uv_index"), NumericField))
	{
		bHasCustomUVIndex = true;
		CustomUVIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, OutputName, bHasCustomUVIndex, CustomUVIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/clear_material_output only supports UMaterial assets"));
		}

		EMaterialProperty OutputProperty = MP_MAX;
		FString ResolvedOutputName;
		if (!ParseMaterialOutputProperty(OutputName, bHasCustomUVIndex, CustomUVIndex, OutputProperty, ResolvedOutputName, Error))
		{
			return MakeFailure(Error);
		}

		FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
		if (!PropertyInput)
		{
			return MakeFailure(FString::Printf(TEXT("Material output is unavailable for this property: %s"), *ResolvedOutputName));
		}

		const bool bWasConnected = PropertyInput->Expression != nullptr;
		const FString PreviousNodeId = bWasConnected ? GetNodeId(PropertyInput->Expression) : FString();
		const int32 PreviousOutputIndex = bWasConnected ? PropertyInput->OutputIndex : INDEX_NONE;

		Context.Material->Modify();
		PropertyInput->Expression = nullptr;
		PropertyInput->OutputIndex = 0;
		PropertyInput->SetMask(0, 0, 0, 0, 0);

		if (bWasConnected)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("output_name"), ResolvedOutputName);
		Result->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
		Result->SetBoolField(TEXT("changed"), bWasConnected);
		Result->SetBoolField(TEXT("was_connected"), bWasConnected);
		Result->SetStringField(TEXT("previous_node_id"), PreviousNodeId);
		if (bWasConnected)
		{
			Result->SetNumberField(TEXT("previous_output_index"), PreviousOutputIndex);
		}

		int32 ResolvedCustomUVIndex = INDEX_NONE;
		if (TryGetCustomUVIndex(OutputProperty, ResolvedCustomUVIndex))
		{
			Result->SetNumberField(TEXT("uv_index"), ResolvedCustomUVIndex);
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleListConnectedOutputs(const FMCPRequest& Request)
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

	bool bIncludeInactive = true;
	Request.Params->TryGetBoolField(TEXT("include_inactive"), bIncludeInactive);

	auto Task = [AssetPath, bIncludeInactive]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/list_connected_outputs only supports UMaterial assets"));
		}

		TArray<EMaterialProperty> OutputProperties;
		AddPhase4MaterialOutputProperties(OutputProperties);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		int32 ConnectedCount = 0;

		for (const EMaterialProperty OutputProperty : OutputProperties)
		{
			const bool bSupported = Context.Material->IsPropertySupported(OutputProperty);
			const bool bActiveInEditor = Context.Material->IsPropertyActiveInEditor(OutputProperty);
			FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
			const bool bConnected = PropertyInput && PropertyInput->Expression != nullptr;

			if (!bIncludeInactive && !bConnected && !bActiveInEditor)
			{
				continue;
			}

			TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
			OutputObj->SetStringField(TEXT("output_name"), MaterialPropertyToOutputName(OutputProperty));
			OutputObj->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
			OutputObj->SetBoolField(TEXT("output_supported"), bSupported);
			OutputObj->SetBoolField(TEXT("output_active_in_editor"), bActiveInEditor);
			OutputObj->SetBoolField(TEXT("output_available"), PropertyInput != nullptr);
			OutputObj->SetBoolField(TEXT("connected"), bConnected);

			int32 CustomUVIndex = INDEX_NONE;
			if (TryGetCustomUVIndex(OutputProperty, CustomUVIndex))
			{
				OutputObj->SetNumberField(TEXT("uv_index"), CustomUVIndex);
			}

			if (bConnected)
			{
				++ConnectedCount;
				UMaterialExpression* SourceNode = PropertyInput->Expression;
				OutputObj->SetStringField(TEXT("from_node_id"), GetNodeId(SourceNode));
				OutputObj->SetStringField(TEXT("from_node_name"), SourceNode->GetName());
				OutputObj->SetStringField(TEXT("from_node_class"), SourceNode->GetClass()->GetName());
				OutputObj->SetNumberField(TEXT("from_output_index"), PropertyInput->OutputIndex);
				OutputObj->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(SourceNode, PropertyInput->OutputIndex, SourceNode->GetOutput(PropertyInput->OutputIndex)));
			}

			Outputs.Add(MakeShared<FJsonValueObject>(OutputObj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetArrayField(TEXT("outputs"), Outputs);
		Result->SetNumberField(TEXT("output_count"), Outputs.Num());
		Result->SetNumberField(TEXT("connected_count"), ConnectedCount);
		Result->SetBoolField(TEXT("include_inactive"), bIncludeInactive);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetCustomUVOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString FromNodeId;
	double UVIndexNumeric = 0.0;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}
	if (!Request.Params->TryGetNumberField(TEXT("uv_index"), UVIndexNumeric))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'uv_index'"));
	}
	const int32 UVIndex = static_cast<int32>(UVIndexNumeric);
	if (UVIndex < 0 || UVIndex > 7)
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'uv_index' must be in range [0, 7]"));
	}

	FString FromOutputPinName;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPinName);
	bool bHasFromOutputIndex = false;
	int32 FromOutputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericField))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, FromNodeId, UVIndex, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_custom_uv_output only supports UMaterial assets"));
		}

		UMaterialExpression* FromNode = FindNodeById(Context, FromNodeId);
		if (!FromNode)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
		}
		if (FromNode->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes cannot drive material outputs"));
		}

		int32 ResolvedFromOutputIndex = INDEX_NONE;
		if (!TryResolveOutputPinIndex(FromNode, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
		{
			return MakeFailure(Error);
		}

		const EMaterialProperty OutputProperty = static_cast<EMaterialProperty>(static_cast<int32>(MP_CustomizedUVs0) + UVIndex);
		FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
		if (!PropertyInput)
		{
			return MakeFailure(FString::Printf(TEXT("Material output is unavailable for custom UV %d"), UVIndex));
		}

		FromNode->Modify();
		Context.Material->Modify();
		FromNode->ConnectExpression(PropertyInput, ResolvedFromOutputIndex);
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("output_name"), MaterialPropertyToOutputName(OutputProperty));
		Result->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
		Result->SetNumberField(TEXT("uv_index"), UVIndex);
		Result->SetStringField(TEXT("from_node_id"), GetNodeId(FromNode));
		Result->SetNumberField(TEXT("from_output_index"), ResolvedFromOutputIndex);
		Result->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(FromNode, ResolvedFromOutputIndex, FromNode->GetOutput(ResolvedFromOutputIndex)));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetPixelDepthOffsetOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString FromNodeId;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}

	FString FromOutputPinName;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPinName);
	bool bHasFromOutputIndex = false;
	int32 FromOutputIndex = 0;
	double NumericField = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericField))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericField);
	}

	auto Task = [AssetPath, FromNodeId, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_pixel_depth_offset_output only supports UMaterial assets"));
		}

		UMaterialExpression* FromNode = FindNodeById(Context, FromNodeId);
		if (!FromNode)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
		}
		if (FromNode->IsA<UMaterialExpressionComment>())
		{
			return MakeFailure(TEXT("Comment nodes cannot drive material outputs"));
		}

		int32 ResolvedFromOutputIndex = INDEX_NONE;
		if (!TryResolveOutputPinIndex(FromNode, FromOutputPinName, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
		{
			return MakeFailure(Error);
		}

		const EMaterialProperty OutputProperty = MP_PixelDepthOffset;
		FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
		if (!PropertyInput)
		{
			return MakeFailure(TEXT("Material output is unavailable for pixel_depth_offset"));
		}

		FromNode->Modify();
		Context.Material->Modify();
		FromNode->ConnectExpression(PropertyInput, ResolvedFromOutputIndex);
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("output_name"), MaterialPropertyToOutputName(OutputProperty));
		Result->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
		Result->SetStringField(TEXT("from_node_id"), GetNodeId(FromNode));
		Result->SetNumberField(TEXT("from_output_index"), ResolvedFromOutputIndex);
		Result->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(FromNode, ResolvedFromOutputIndex, FromNode->GetOutput(ResolvedFromOutputIndex)));
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
		PhasesObj->SetBoolField(TEXT("phase_3_pin_wiring_operations"), true);
		PhasesObj->SetBoolField(TEXT("phase_4_material_output_authoring"), true);
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
