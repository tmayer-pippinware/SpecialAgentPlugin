// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"

#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "GameThreadDispatcher.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/MaterialExpressionFontSampleParameter.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionRuntimeVirtualTextureSampleParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticComponentMaskParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/Font.h"
#include "Engine/Texture.h"
#include "Misc/DataValidation.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"

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

	enum class EMaterialParameterNodeType : uint8
	{
		Unknown,
		Scalar,
		Vector,
		Texture,
		StaticSwitch,
		StaticComponentMask,
		RuntimeVirtualTexture,
		Font
	};

	static FString NormalizeParameterToken(const FString& Input)
	{
		FString Token = Input.TrimStartAndEnd().ToLower();
		Token = Token.Replace(TEXT(" "), TEXT("_"));
		Token = Token.Replace(TEXT("-"), TEXT("_"));
		return Token;
	}

	static bool ParseParameterTypeToken(const FString& Input, EMaterialParameterNodeType& OutType)
	{
		const FString Token = NormalizeParameterToken(Input);
		if (Token == TEXT("scalar") || Token == TEXT("float"))
		{
			OutType = EMaterialParameterNodeType::Scalar;
			return true;
		}
		if (Token == TEXT("vector") || Token == TEXT("color"))
		{
			OutType = EMaterialParameterNodeType::Vector;
			return true;
		}
		if (Token == TEXT("texture") || Token == TEXT("texture2d"))
		{
			OutType = EMaterialParameterNodeType::Texture;
			return true;
		}
		if (Token == TEXT("static_switch") || Token == TEXT("switch"))
		{
			OutType = EMaterialParameterNodeType::StaticSwitch;
			return true;
		}
		if (Token == TEXT("static_component_mask") || Token == TEXT("component_mask") || Token == TEXT("mask"))
		{
			OutType = EMaterialParameterNodeType::StaticComponentMask;
			return true;
		}
		if (Token == TEXT("runtime_virtual_texture") || Token == TEXT("virtual_texture") || Token == TEXT("rvt"))
		{
			OutType = EMaterialParameterNodeType::RuntimeVirtualTexture;
			return true;
		}
		if (Token == TEXT("font"))
		{
			OutType = EMaterialParameterNodeType::Font;
			return true;
		}
		return false;
	}

	static FString ParameterTypeToString(const EMaterialParameterNodeType Type)
	{
		switch (Type)
		{
		case EMaterialParameterNodeType::Scalar: return TEXT("scalar");
		case EMaterialParameterNodeType::Vector: return TEXT("vector");
		case EMaterialParameterNodeType::Texture: return TEXT("texture");
		case EMaterialParameterNodeType::StaticSwitch: return TEXT("static_switch");
		case EMaterialParameterNodeType::StaticComponentMask: return TEXT("static_component_mask");
		case EMaterialParameterNodeType::RuntimeVirtualTexture: return TEXT("runtime_virtual_texture");
		case EMaterialParameterNodeType::Font: return TEXT("font");
		default: return TEXT("unknown");
		}
	}

	static EMaterialParameterNodeType GetParameterNodeType(const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return EMaterialParameterNodeType::Unknown;
		}
		if (Expression->IsA<UMaterialExpressionScalarParameter>())
		{
			return EMaterialParameterNodeType::Scalar;
		}
		if (Expression->IsA<UMaterialExpressionVectorParameter>())
		{
			return EMaterialParameterNodeType::Vector;
		}
		if (Expression->IsA<UMaterialExpressionTextureSampleParameter>())
		{
			return EMaterialParameterNodeType::Texture;
		}
		if (Expression->IsA<UMaterialExpressionStaticSwitchParameter>())
		{
			return EMaterialParameterNodeType::StaticSwitch;
		}
		if (Expression->IsA<UMaterialExpressionStaticComponentMaskParameter>())
		{
			return EMaterialParameterNodeType::StaticComponentMask;
		}
		if (Expression->IsA<UMaterialExpressionRuntimeVirtualTextureSampleParameter>())
		{
			return EMaterialParameterNodeType::RuntimeVirtualTexture;
		}
		if (Expression->IsA<UMaterialExpressionFontSampleParameter>())
		{
			return EMaterialParameterNodeType::Font;
		}
		return EMaterialParameterNodeType::Unknown;
	}

	static bool IsSupportedParameterExpression(const UMaterialExpression* Expression)
	{
		return GetParameterNodeType(Expression) != EMaterialParameterNodeType::Unknown;
	}

	static FName GetParameterExpressionName(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			return Parameter->ParameterName;
		}
		if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			return TextureParameter->ParameterName;
		}
		if (const UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			return RuntimeVirtualTextureParameter->ParameterName;
		}
		if (const UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			return FontParameter->ParameterName;
		}
		return NAME_None;
	}

	static bool SetParameterExpressionName(UMaterialExpression* Expression, const FName NewName)
	{
		if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			Parameter->ParameterName = NewName;
			return true;
		}
		if (UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			TextureParameter->ParameterName = NewName;
			return true;
		}
		if (UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			RuntimeVirtualTextureParameter->ParameterName = NewName;
			return true;
		}
		if (UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			FontParameter->ParameterName = NewName;
			return true;
		}
		return false;
	}

	static FName GetParameterExpressionGroup(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			return Parameter->Group;
		}
		if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			return TextureParameter->Group;
		}
		if (const UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			return RuntimeVirtualTextureParameter->Group;
		}
		if (const UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			return FontParameter->Group;
		}
		return NAME_None;
	}

	static bool SetParameterExpressionGroup(UMaterialExpression* Expression, const FName GroupName)
	{
		if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			Parameter->Group = GroupName;
			return true;
		}
		if (UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			TextureParameter->Group = GroupName;
			return true;
		}
		if (UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			RuntimeVirtualTextureParameter->Group = GroupName;
			return true;
		}
		if (UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			FontParameter->Group = GroupName;
			return true;
		}
		return false;
	}

	static int32 GetParameterExpressionSortPriority(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			return Parameter->SortPriority;
		}
		if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			return TextureParameter->SortPriority;
		}
		if (const UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			return RuntimeVirtualTextureParameter->SortPriority;
		}
		if (const UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			return FontParameter->SortPriority;
		}
		return 0;
	}

	static bool SetParameterExpressionSortPriority(UMaterialExpression* Expression, const int32 SortPriority)
	{
		if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			Parameter->SortPriority = SortPriority;
			return true;
		}
		if (UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			TextureParameter->SortPriority = SortPriority;
			return true;
		}
		if (UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			RuntimeVirtualTextureParameter->SortPriority = SortPriority;
			return true;
		}
		if (UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			FontParameter->SortPriority = SortPriority;
			return true;
		}
		return false;
	}

	static FGuid GetParameterExpressionGuid(const UMaterialExpression* Expression)
	{
		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			return Parameter->ExpressionGUID;
		}
		if (const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			return TextureParameter->ExpressionGUID;
		}
		if (const UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			return RuntimeVirtualTextureParameter->ExpressionGUID;
		}
		if (const UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			return FontParameter->ExpressionGUID;
		}
		return FGuid();
	}

	static bool SetParameterExpressionGuid(UMaterialExpression* Expression, const FGuid& Guid)
	{
		if (UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			Parameter->ExpressionGUID = Guid;
			return true;
		}
		if (UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			TextureParameter->ExpressionGUID = Guid;
			return true;
		}
		if (UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTextureParameter = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			RuntimeVirtualTextureParameter->ExpressionGUID = Guid;
			return true;
		}
		if (UMaterialExpressionFontSampleParameter* FontParameter = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			FontParameter->ExpressionGUID = Guid;
			return true;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("r"), Color.R);
		Value->SetNumberField(TEXT("g"), Color.G);
		Value->SetNumberField(TEXT("b"), Color.B);
		Value->SetNumberField(TEXT("a"), Color.A);
		return Value;
	}

	static TSharedPtr<FJsonObject> BuildMaskJson(const bool bR, const bool bG, const bool bB, const bool bA)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("r"), bR);
		Value->SetBoolField(TEXT("g"), bG);
		Value->SetBoolField(TEXT("b"), bB);
		Value->SetBoolField(TEXT("a"), bA);
		return Value;
	}

	static TSharedPtr<FJsonObject> BuildChannelNamesJson(const FParameterChannelNames& ChannelNames)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("r"), ChannelNames.R.ToString());
		Value->SetStringField(TEXT("g"), ChannelNames.G.ToString());
		Value->SetStringField(TEXT("b"), ChannelNames.B.ToString());
		Value->SetStringField(TEXT("a"), ChannelNames.A.ToString());
		return Value;
	}

	static bool TryApplyChannelNameFields(
		const TSharedPtr<FJsonObject>& Params,
		FParameterChannelNames& InOutChannelNames,
		bool& bOutAnyFieldsSet)
	{
		bOutAnyFieldsSet = false;
		if (!Params.IsValid())
		{
			return false;
		}

		auto ApplyField = [&InOutChannelNames, &bOutAnyFieldsSet](const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, FText& Target) -> void
		{
			if (!Source.IsValid())
			{
				return;
			}

			FString Value;
			if (Source->TryGetStringField(FieldName, Value))
			{
				Target = FText::FromString(Value);
				bOutAnyFieldsSet = true;
			}
		};

		const TSharedPtr<FJsonObject>* ChannelNamesObjectPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("channel_names"), ChannelNamesObjectPtr) && ChannelNamesObjectPtr && ChannelNamesObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& ChannelNamesObject = *ChannelNamesObjectPtr;
			ApplyField(ChannelNamesObject, TEXT("r"), InOutChannelNames.R);
			ApplyField(ChannelNamesObject, TEXT("g"), InOutChannelNames.G);
			ApplyField(ChannelNamesObject, TEXT("b"), InOutChannelNames.B);
			ApplyField(ChannelNamesObject, TEXT("a"), InOutChannelNames.A);
		}

		ApplyField(Params, TEXT("channel_r"), InOutChannelNames.R);
		ApplyField(Params, TEXT("channel_g"), InOutChannelNames.G);
		ApplyField(Params, TEXT("channel_b"), InOutChannelNames.B);
		ApplyField(Params, TEXT("channel_a"), InOutChannelNames.A);

		return true;
	}

	static bool TryReadLinearColor(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		FLinearColor& OutColor,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		double ScalarValue = 0.0;
		if (Params->TryGetNumberField(FieldName, ScalarValue))
		{
			const float Value = static_cast<float>(ScalarValue);
			OutColor = FLinearColor(Value, Value, Value, 1.0f);
			return true;
		}

		FString ColorString;
		if (Params->TryGetStringField(FieldName, ColorString))
		{
			if (OutColor.InitFromString(ColorString))
			{
				return true;
			}
			OutError = FString::Printf(TEXT("Failed to parse color string '%s'"), *ColorString);
			return false;
		}

		const TSharedPtr<FJsonObject>* ColorObjectPtr = nullptr;
		if (Params->TryGetObjectField(FieldName, ColorObjectPtr) && ColorObjectPtr && ColorObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& ColorObject = *ColorObjectPtr;
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			const bool bHasR = ColorObject->TryGetNumberField(TEXT("r"), R);
			const bool bHasG = ColorObject->TryGetNumberField(TEXT("g"), G);
			const bool bHasB = ColorObject->TryGetNumberField(TEXT("b"), B);
			ColorObject->TryGetNumberField(TEXT("a"), A);
			if (!bHasR || !bHasG || !bHasB)
			{
				OutError = FString::Printf(TEXT("Field '%s' requires at least r, g, and b"), FieldName);
				return false;
			}
			OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
			return true;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		const bool bHasR = Params->TryGetNumberField(TEXT("default_r"), R);
		const bool bHasG = Params->TryGetNumberField(TEXT("default_g"), G);
		const bool bHasB = Params->TryGetNumberField(TEXT("default_b"), B);
		if (bHasR && bHasG && bHasB)
		{
			Params->TryGetNumberField(TEXT("default_a"), A);
			OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
			return true;
		}

		OutError = FString::Printf(TEXT("Missing required color field '%s'"), FieldName);
		return false;
	}

	static bool TryReadStaticComponentMaskDefaults(
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutR,
		bool& bOutG,
		bool& bOutB,
		bool& bOutA,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		const TSharedPtr<FJsonObject>* MaskObjectPtr = nullptr;
		if ((Params->TryGetObjectField(TEXT("default_mask"), MaskObjectPtr) || Params->TryGetObjectField(TEXT("default_value"), MaskObjectPtr)) && MaskObjectPtr && MaskObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& MaskObject = *MaskObjectPtr;

			const bool bHasR = MaskObject->TryGetBoolField(TEXT("r"), bOutR);
			const bool bHasG = MaskObject->TryGetBoolField(TEXT("g"), bOutG);
			const bool bHasB = MaskObject->TryGetBoolField(TEXT("b"), bOutB);
			const bool bHasA = MaskObject->TryGetBoolField(TEXT("a"), bOutA);
			if (!(bHasR && bHasG && bHasB && bHasA))
			{
				OutError = TEXT("Mask object must include boolean r, g, b, and a fields");
				return false;
			}
			return true;
		}

		const bool bHasR = Params->TryGetBoolField(TEXT("default_r"), bOutR);
		const bool bHasG = Params->TryGetBoolField(TEXT("default_g"), bOutG);
		const bool bHasB = Params->TryGetBoolField(TEXT("default_b"), bOutB);
		const bool bHasA = Params->TryGetBoolField(TEXT("default_a"), bOutA);
		if (bHasR && bHasG && bHasB && bHasA)
		{
			return true;
		}

		OutError = TEXT("Missing static component mask defaults; provide default_mask{r,g,b,a} or default_r/default_g/default_b/default_a");
		return false;
	}

	static void GatherParameterExpressions(const FMaterialGraphContext& Context, TArray<UMaterialExpression*>& OutParameters)
	{
		OutParameters.Reset();
		if (!Context.Material)
		{
			return;
		}

		for (UMaterialExpression* Expression : Context.Material->GetExpressions())
		{
			if (IsSupportedParameterExpression(Expression))
			{
				OutParameters.Add(Expression);
			}
		}
	}

	static bool ParameterNameEquals(const FName NameA, const FString& NameB)
	{
		return NameA.ToString().Equals(NameB, ESearchCase::IgnoreCase);
	}

	static void GatherParameterMatchesByName(
		const FMaterialGraphContext& Context,
		const FString& ParameterName,
		const bool bHasTypeFilter,
		const EMaterialParameterNodeType TypeFilter,
		TArray<UMaterialExpression*>& OutMatches)
	{
		OutMatches.Reset();
		const FString TrimmedName = ParameterName.TrimStartAndEnd();
		if (TrimmedName.IsEmpty())
		{
			return;
		}

		TArray<UMaterialExpression*> Parameters;
		GatherParameterExpressions(Context, Parameters);
		for (UMaterialExpression* Expression : Parameters)
		{
			if (!Expression)
			{
				continue;
			}

			if (!ParameterNameEquals(GetParameterExpressionName(Expression), TrimmedName))
			{
				continue;
			}

			const EMaterialParameterNodeType ParameterType = GetParameterNodeType(Expression);
			if (bHasTypeFilter && ParameterType != TypeFilter)
			{
				continue;
			}

			OutMatches.Add(Expression);
		}
	}

	static UMaterialExpression* ResolveParameterExpression(
		const FMaterialGraphContext& Context,
		const FString& NodeId,
		const FString& ParameterName,
		const bool bHasTypeFilter,
		const EMaterialParameterNodeType TypeFilter,
		FString& OutError)
	{
		const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
		if (!TrimmedNodeId.IsEmpty())
		{
			UMaterialExpression* Expression = FindNodeById(Context, TrimmedNodeId);
			if (!Expression)
			{
				OutError = FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId);
				return nullptr;
			}
			if (!IsSupportedParameterExpression(Expression))
			{
				OutError = FString::Printf(TEXT("Node is not a supported parameter expression: %s"), *TrimmedNodeId);
				return nullptr;
			}
			if (bHasTypeFilter && GetParameterNodeType(Expression) != TypeFilter)
			{
				OutError = FString::Printf(TEXT("Node is not of requested parameter_type '%s'"), *ParameterTypeToString(TypeFilter));
				return nullptr;
			}
			return Expression;
		}

		TArray<UMaterialExpression*> Matches;
		GatherParameterMatchesByName(Context, ParameterName, bHasTypeFilter, TypeFilter, Matches);
		if (Matches.Num() == 0)
		{
			OutError = ParameterName.TrimStartAndEnd().IsEmpty()
				? TEXT("Missing target parameter: provide node_id or parameter_name")
				: FString::Printf(TEXT("Parameter not found: %s"), *ParameterName);
			return nullptr;
		}
		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Parameter name is ambiguous (%d matches). Provide node_id."), Matches.Num());
			return nullptr;
		}
		return Matches[0];
	}

	static TSharedPtr<FJsonObject> BuildParameterJson(UMaterialExpression* Expression)
	{
		TSharedPtr<FJsonObject> ParameterObject = BuildNodeJson(Expression);
		if (!Expression || !ParameterObject.IsValid())
		{
			return ParameterObject;
		}

		const EMaterialParameterNodeType ParameterType = GetParameterNodeType(Expression);
		ParameterObject->SetStringField(TEXT("parameter_type"), ParameterTypeToString(ParameterType));
		ParameterObject->SetStringField(TEXT("parameter_name"), GetParameterExpressionName(Expression).ToString());
		ParameterObject->SetStringField(TEXT("group"), GetParameterExpressionGroup(Expression).ToString());
		ParameterObject->SetNumberField(TEXT("sort_priority"), GetParameterExpressionSortPriority(Expression));
		ParameterObject->SetStringField(TEXT("description"), Expression->Desc);

		const FGuid ParameterGuid = GetParameterExpressionGuid(Expression);
		ParameterObject->SetStringField(TEXT("parameter_guid"), ParameterGuid.IsValid() ? ParameterGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());

		if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			ParameterObject->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
		}
		else if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			ParameterObject->SetObjectField(TEXT("default_value"), BuildColorJson(Vector->DefaultValue));
			ParameterObject->SetObjectField(TEXT("channel_names"), BuildChannelNamesJson(Vector->ChannelNames));
		}
		else if (const UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
		{
			ParameterObject->SetStringField(TEXT("default_texture"), Texture->Texture ? Texture->Texture->GetPathName() : FString());
			ParameterObject->SetObjectField(TEXT("channel_names"), BuildChannelNamesJson(Texture->ChannelNames));
		}
		else if (const UMaterialExpressionStaticSwitchParameter* StaticSwitch = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
		{
			ParameterObject->SetBoolField(TEXT("default_value"), StaticSwitch->DefaultValue != 0);
		}
		else if (const UMaterialExpressionStaticComponentMaskParameter* StaticMask = Cast<UMaterialExpressionStaticComponentMaskParameter>(Expression))
		{
			ParameterObject->SetObjectField(TEXT("default_mask"), BuildMaskJson(StaticMask->DefaultR != 0, StaticMask->DefaultG != 0, StaticMask->DefaultB != 0, StaticMask->DefaultA != 0));
		}
		else if (const UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTexture = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expression))
		{
			ParameterObject->SetStringField(TEXT("default_runtime_virtual_texture"), RuntimeVirtualTexture->VirtualTexture ? RuntimeVirtualTexture->VirtualTexture->GetPathName() : FString());
		}
		else if (const UMaterialExpressionFontSampleParameter* Font = Cast<UMaterialExpressionFontSampleParameter>(Expression))
		{
			ParameterObject->SetStringField(TEXT("default_font"), Font->Font ? Font->Font->GetPathName() : FString());
			ParameterObject->SetNumberField(TEXT("font_page"), Font->FontTexturePage);
		}

		return ParameterObject;
	}

	static bool DoesParameterNameConflict(
		const FMaterialGraphContext& Context,
		const UMaterialExpression* IgnoredExpression,
		const FName CandidateName,
		FString& OutConflictingNodeId)
	{
		TArray<UMaterialExpression*> Parameters;
		GatherParameterExpressions(Context, Parameters);
		for (UMaterialExpression* Existing : Parameters)
		{
			if (!Existing || Existing == IgnoredExpression)
			{
				continue;
			}

			if (GetParameterExpressionName(Existing).IsEqual(CandidateName, ENameCase::IgnoreCase))
			{
				OutConflictingNodeId = GetNodeId(Existing);
				return true;
			}
		}

		return false;
	}

	static bool TryResolveParameterTypeFilter(
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutHasTypeFilter,
		EMaterialParameterNodeType& OutTypeFilter,
		FString& OutError)
	{
		bOutHasTypeFilter = false;
		OutTypeFilter = EMaterialParameterNodeType::Unknown;
		if (!Params.IsValid())
		{
			return true;
		}

		FString ParameterTypeString;
		if (!Params->TryGetStringField(TEXT("parameter_type"), ParameterTypeString))
		{
			return true;
		}

		if (!ParseParameterTypeToken(ParameterTypeString, OutTypeFilter))
		{
			OutError = FString::Printf(TEXT("Unsupported parameter_type '%s'"), *ParameterTypeString);
			return false;
		}

		bOutHasTypeFilter = true;
		return true;
	}

	enum class EMaterialInstanceOverrideType : uint8
	{
		Unknown,
		Scalar,
		Vector,
		Texture,
		StaticSwitch,
		StaticComponentMask
	};

	static FString MaterialParameterAssociationToString(const EMaterialParameterAssociation Association)
	{
		switch (Association)
		{
		case EMaterialParameterAssociation::GlobalParameter:
			return TEXT("global");
		case EMaterialParameterAssociation::LayerParameter:
			return TEXT("layer");
		case EMaterialParameterAssociation::BlendParameter:
			return TEXT("blend");
		default:
			return TEXT("unknown");
		}
	}

	static bool ParseOptionalMaterialParameterAssociationAndIndex(
		const TSharedPtr<FJsonObject>& Params,
		EMaterialParameterAssociation& OutAssociation,
		int32& OutIndex,
		bool& bOutHasAssociation,
		bool& bOutHasIndex,
		FString& OutError)
	{
		OutAssociation = EMaterialParameterAssociation::GlobalParameter;
		OutIndex = INDEX_NONE;
		bOutHasAssociation = false;
		bOutHasIndex = false;

		if (!Params.IsValid())
		{
			return true;
		}

		double IndexNumeric = 0.0;
		if (Params->TryGetNumberField(TEXT("index"), IndexNumeric) ||
			Params->TryGetNumberField(TEXT("layer_index"), IndexNumeric))
		{
			bOutHasIndex = true;
			OutIndex = static_cast<int32>(IndexNumeric);
		}

		FString AssociationString;
		if (!Params->TryGetStringField(TEXT("association"), AssociationString))
		{
			return true;
		}

		const FString NormalizedAssociation = NormalizeParameterToken(AssociationString);
		bOutHasAssociation = true;
		if (NormalizedAssociation == TEXT("global") || NormalizedAssociation == TEXT("global_parameter"))
		{
			OutAssociation = EMaterialParameterAssociation::GlobalParameter;
			if (!bOutHasIndex)
			{
				OutIndex = INDEX_NONE;
			}
			return true;
		}
		if (NormalizedAssociation == TEXT("layer") || NormalizedAssociation == TEXT("layer_parameter"))
		{
			OutAssociation = EMaterialParameterAssociation::LayerParameter;
			if (!bOutHasIndex)
			{
				OutIndex = 0;
			}
			return true;
		}
		if (NormalizedAssociation == TEXT("blend") || NormalizedAssociation == TEXT("blend_parameter"))
		{
			OutAssociation = EMaterialParameterAssociation::BlendParameter;
			if (!bOutHasIndex)
			{
				OutIndex = 0;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported association '%s'"), *AssociationString);
		return false;
	}

	static bool ResolveMaterialParameterAssociationAndIndexForSet(
		const TSharedPtr<FJsonObject>& Params,
		EMaterialParameterAssociation& OutAssociation,
		int32& OutIndex,
		FString& OutError)
	{
		bool bHasAssociation = false;
		bool bHasIndex = false;
		if (!ParseOptionalMaterialParameterAssociationAndIndex(Params, OutAssociation, OutIndex, bHasAssociation, bHasIndex, OutError))
		{
			return false;
		}

		if (!bHasAssociation)
		{
			OutAssociation = EMaterialParameterAssociation::GlobalParameter;
			OutIndex = INDEX_NONE;
		}

		if (!bHasIndex)
		{
			if (OutAssociation == EMaterialParameterAssociation::LayerParameter ||
				OutAssociation == EMaterialParameterAssociation::BlendParameter)
			{
				OutIndex = 0;
			}
			else
			{
				OutIndex = INDEX_NONE;
			}
		}

		return true;
	}

	static bool ParseOptionalMaterialInstanceOverrideType(
		const TSharedPtr<FJsonObject>& Params,
		EMaterialInstanceOverrideType& OutType,
		bool& bOutHasType,
		FString& OutError)
	{
		OutType = EMaterialInstanceOverrideType::Unknown;
		bOutHasType = false;
		if (!Params.IsValid())
		{
			return true;
		}

		FString TypeString;
		if (!Params->TryGetStringField(TEXT("parameter_type"), TypeString) &&
			!Params->TryGetStringField(TEXT("override_type"), TypeString))
		{
			return true;
		}

		const FString Token = NormalizeParameterToken(TypeString);
		bOutHasType = true;
		if (Token == TEXT("scalar") || Token == TEXT("float"))
		{
			OutType = EMaterialInstanceOverrideType::Scalar;
			return true;
		}
		if (Token == TEXT("vector") || Token == TEXT("color"))
		{
			OutType = EMaterialInstanceOverrideType::Vector;
			return true;
		}
		if (Token == TEXT("texture") || Token == TEXT("texture2d"))
		{
			OutType = EMaterialInstanceOverrideType::Texture;
			return true;
		}
		if (Token == TEXT("static_switch") || Token == TEXT("switch"))
		{
			OutType = EMaterialInstanceOverrideType::StaticSwitch;
			return true;
		}
		if (Token == TEXT("static_component_mask") || Token == TEXT("component_mask") || Token == TEXT("mask"))
		{
			OutType = EMaterialInstanceOverrideType::StaticComponentMask;
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported override type '%s'"), *TypeString);
		return false;
	}

	static FString MaterialInstanceOverrideTypeToString(const EMaterialInstanceOverrideType Type)
	{
		switch (Type)
		{
		case EMaterialInstanceOverrideType::Scalar:
			return TEXT("scalar");
		case EMaterialInstanceOverrideType::Vector:
			return TEXT("vector");
		case EMaterialInstanceOverrideType::Texture:
			return TEXT("texture");
		case EMaterialInstanceOverrideType::StaticSwitch:
			return TEXT("static_switch");
		case EMaterialInstanceOverrideType::StaticComponentMask:
			return TEXT("static_component_mask");
		default:
			return TEXT("unknown");
		}
	}

	static TSharedPtr<FJsonObject> BuildMaterialParameterInfoJson(const FMaterialParameterInfo& ParameterInfo)
	{
		TSharedPtr<FJsonObject> InfoObject = MakeShared<FJsonObject>();
		InfoObject->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
		InfoObject->SetStringField(TEXT("association"), MaterialParameterAssociationToString(ParameterInfo.Association));
		InfoObject->SetNumberField(TEXT("index"), ParameterInfo.Index);
		return InfoObject;
	}

	static bool MatchesMaterialParameterInfo(
		const FMaterialParameterInfo& ParameterInfo,
		const FString& ParameterName,
		const bool bHasAssociation,
		const EMaterialParameterAssociation Association,
		const bool bHasIndex,
		const int32 Index)
	{
		if (!ParameterInfo.Name.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (bHasAssociation && ParameterInfo.Association != Association)
		{
			return false;
		}
		if (bHasIndex && ParameterInfo.Index != Index)
		{
			return false;
		}
		return true;
	}

	static UMaterialInstanceConstant* ResolveMaterialInstance(const FString& InputPath, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid material instance path: %s"), *InputPath);
			return nullptr;
		}

		UMaterialInstanceConstant* Instance = LoadAssetAs<UMaterialInstanceConstant>(OutAssetPath);
		if (!Instance)
		{
			OutError = FString::Printf(TEXT("Material instance not found: %s"), *OutAssetPath);
			return nullptr;
		}

		return Instance;
	}

	static TSharedPtr<FJsonObject> BuildScalarOverrideJson(const FScalarParameterValue& Value)
	{
		TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
		OverrideObject->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		OverrideObject->SetStringField(TEXT("parameter_name"), Value.ParameterInfo.Name.ToString());
		OverrideObject->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(Value.ParameterInfo));
		OverrideObject->SetNumberField(TEXT("value"), Value.ParameterValue);
		return OverrideObject;
	}

	static TSharedPtr<FJsonObject> BuildVectorOverrideJson(const FVectorParameterValue& Value)
	{
		TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
		OverrideObject->SetStringField(TEXT("parameter_type"), TEXT("vector"));
		OverrideObject->SetStringField(TEXT("parameter_name"), Value.ParameterInfo.Name.ToString());
		OverrideObject->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(Value.ParameterInfo));
		OverrideObject->SetObjectField(TEXT("value"), BuildColorJson(Value.ParameterValue));
		return OverrideObject;
	}

	static TSharedPtr<FJsonObject> BuildTextureOverrideJson(const FTextureParameterValue& Value)
	{
		TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
		OverrideObject->SetStringField(TEXT("parameter_type"), TEXT("texture"));
		OverrideObject->SetStringField(TEXT("parameter_name"), Value.ParameterInfo.Name.ToString());
		OverrideObject->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(Value.ParameterInfo));
		OverrideObject->SetStringField(TEXT("value"), Value.ParameterValue ? Value.ParameterValue->GetPathName() : FString());
		return OverrideObject;
	}

	static TSharedPtr<FJsonObject> BuildStaticSwitchOverrideJson(const FStaticSwitchParameter& Value)
	{
		TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
		OverrideObject->SetStringField(TEXT("parameter_type"), TEXT("static_switch"));
		OverrideObject->SetStringField(TEXT("parameter_name"), Value.ParameterInfo.Name.ToString());
		OverrideObject->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(Value.ParameterInfo));
		OverrideObject->SetBoolField(TEXT("value"), Value.Value);
		OverrideObject->SetBoolField(TEXT("is_override"), Value.bOverride);
		OverrideObject->SetStringField(TEXT("expression_guid"), Value.ExpressionGUID.IsValid() ? Value.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		return OverrideObject;
	}

	static TSharedPtr<FJsonObject> BuildStaticComponentMaskOverrideJson(const FStaticComponentMaskParameter& Value)
	{
		TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
		OverrideObject->SetStringField(TEXT("parameter_type"), TEXT("static_component_mask"));
		OverrideObject->SetStringField(TEXT("parameter_name"), Value.ParameterInfo.Name.ToString());
		OverrideObject->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(Value.ParameterInfo));
		OverrideObject->SetObjectField(TEXT("value"), BuildMaskJson(Value.R, Value.G, Value.B, Value.A));
		OverrideObject->SetBoolField(TEXT("is_override"), Value.bOverride);
		OverrideObject->SetStringField(TEXT("expression_guid"), Value.ExpressionGUID.IsValid() ? Value.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		return OverrideObject;
	}

	static bool TryReadMaterialInstanceMaskValue(
		const TSharedPtr<FJsonObject>& Params,
		bool& OutR,
		bool& OutG,
		bool& OutB,
		bool& OutA,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		const TSharedPtr<FJsonObject>* ValueObject = nullptr;
		if ((Params->TryGetObjectField(TEXT("value"), ValueObject) || Params->TryGetObjectField(TEXT("value_mask"), ValueObject)) && ValueObject && ValueObject->IsValid())
		{
			const TSharedPtr<FJsonObject>& MaskObject = *ValueObject;
			const bool bHasR = MaskObject->TryGetBoolField(TEXT("r"), OutR);
			const bool bHasG = MaskObject->TryGetBoolField(TEXT("g"), OutG);
			const bool bHasB = MaskObject->TryGetBoolField(TEXT("b"), OutB);
			const bool bHasA = MaskObject->TryGetBoolField(TEXT("a"), OutA);
			if (!(bHasR && bHasG && bHasB && bHasA))
			{
				OutError = TEXT("Mask object must include boolean r, g, b, and a fields");
				return false;
			}
			return true;
		}

		const bool bHasR = Params->TryGetBoolField(TEXT("value_r"), OutR) || Params->TryGetBoolField(TEXT("r"), OutR);
		const bool bHasG = Params->TryGetBoolField(TEXT("value_g"), OutG) || Params->TryGetBoolField(TEXT("g"), OutG);
		const bool bHasB = Params->TryGetBoolField(TEXT("value_b"), OutB) || Params->TryGetBoolField(TEXT("b"), OutB);
		const bool bHasA = Params->TryGetBoolField(TEXT("value_a"), OutA) || Params->TryGetBoolField(TEXT("a"), OutA);
		if (bHasR && bHasG && bHasB && bHasA)
		{
			return true;
		}

		OutError = TEXT("Missing mask value; provide value{r,g,b,a} or value_r/value_g/value_b/value_a");
		return false;
	}

	static UMaterialFunction* ResolveMaterialFunctionAsset(const FString& InputPath, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid material function path: %s"), *InputPath);
			return nullptr;
		}

		UMaterialFunction* Function = LoadAssetAs<UMaterialFunction>(OutAssetPath);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Material function not found: %s"), *OutAssetPath);
			return nullptr;
		}

		return Function;
	}

	static void GatherMaterialFunctionIONodes(
		UMaterialFunction* Function,
		TArray<UMaterialExpressionFunctionInput*>& OutInputs,
		TArray<UMaterialExpressionFunctionOutput*>& OutOutputs)
	{
		OutInputs.Reset();
		OutOutputs.Reset();
		if (!Function)
		{
			return;
		}

		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				OutInputs.Add(FunctionInput);
				continue;
			}
			if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				OutOutputs.Add(FunctionOutput);
			}
		}
	}

	static FString FunctionInputTypeToString(const EFunctionInputType InputType)
	{
		switch (InputType)
		{
		case FunctionInput_Scalar:
			return TEXT("scalar");
		case FunctionInput_Vector2:
			return TEXT("vector2");
		case FunctionInput_Vector3:
			return TEXT("vector3");
		case FunctionInput_Vector4:
			return TEXT("vector4");
		case FunctionInput_Texture2D:
			return TEXT("texture2d");
		case FunctionInput_TextureCube:
			return TEXT("texturecube");
		case FunctionInput_Texture2DArray:
			return TEXT("texture2darray");
		case FunctionInput_VolumeTexture:
			return TEXT("volumetexture");
		case FunctionInput_StaticBool:
			return TEXT("staticbool");
		case FunctionInput_MaterialAttributes:
			return TEXT("materialattributes");
		case FunctionInput_TextureExternal:
			return TEXT("textureexternal");
		case FunctionInput_Bool:
			return TEXT("bool");
		case FunctionInput_Substrate:
			return TEXT("substrate");
		default:
			return TEXT("unknown");
		}
	}

	static FString MaterialValueTypeToString(const EMaterialValueType ValueType)
	{
		switch (ValueType)
		{
		case MCT_Float:
		case MCT_Float1:
			return TEXT("float");
		case MCT_Float2:
			return TEXT("float2");
		case MCT_Float3:
			return TEXT("float3");
		case MCT_Float4:
			return TEXT("float4");
		case MCT_Texture:
		case MCT_Texture2D:
			return TEXT("texture2d");
		case MCT_TextureCube:
			return TEXT("texturecube");
		case MCT_Texture2DArray:
			return TEXT("texture2darray");
		case MCT_TextureExternal:
			return TEXT("textureexternal");
		case MCT_VolumeTexture:
			return TEXT("volumetexture");
		case MCT_StaticBool:
			return TEXT("staticbool");
		case MCT_Bool:
			return TEXT("bool");
		case MCT_MaterialAttributes:
			return TEXT("materialattributes");
		case MCT_Substrate:
			return TEXT("substrate");
		default:
			break;
		}

		return FString::Printf(TEXT("value_%d"), static_cast<int32>(ValueType));
	}

	static bool ParseFunctionInputTypeToken(const FString& Input, EFunctionInputType& OutType, FString& OutError)
	{
		const FString Token = NormalizeParameterToken(Input);
		if (Token.IsEmpty())
		{
			OutError = TEXT("Missing function input type");
			return false;
		}

		if (Token == TEXT("scalar") || Token == TEXT("float") || Token == TEXT("float1"))
		{
			OutType = FunctionInput_Scalar;
			return true;
		}
		if (Token == TEXT("vector2") || Token == TEXT("float2"))
		{
			OutType = FunctionInput_Vector2;
			return true;
		}
		if (Token == TEXT("vector3") || Token == TEXT("float3") || Token == TEXT("vector"))
		{
			OutType = FunctionInput_Vector3;
			return true;
		}
		if (Token == TEXT("vector4") || Token == TEXT("float4") || Token == TEXT("color"))
		{
			OutType = FunctionInput_Vector4;
			return true;
		}
		if (Token == TEXT("texture") || Token == TEXT("texture2d"))
		{
			OutType = FunctionInput_Texture2D;
			return true;
		}
		if (Token == TEXT("texturecube") || Token == TEXT("cube"))
		{
			OutType = FunctionInput_TextureCube;
			return true;
		}
		if (Token == TEXT("texture2darray"))
		{
			OutType = FunctionInput_Texture2DArray;
			return true;
		}
		if (Token == TEXT("volumetexture"))
		{
			OutType = FunctionInput_VolumeTexture;
			return true;
		}
		if (Token == TEXT("staticbool") || Token == TEXT("static_switch"))
		{
			OutType = FunctionInput_StaticBool;
			return true;
		}
		if (Token == TEXT("materialattributes") || Token == TEXT("material_attributes"))
		{
			OutType = FunctionInput_MaterialAttributes;
			return true;
		}
		if (Token == TEXT("textureexternal"))
		{
			OutType = FunctionInput_TextureExternal;
			return true;
		}
		if (Token == TEXT("bool"))
		{
			OutType = FunctionInput_Bool;
			return true;
		}
		if (Token == TEXT("substrate"))
		{
			OutType = FunctionInput_Substrate;
			return true;
		}

		const UEnum* Enum = StaticEnum<EFunctionInputType>();
		if (Enum)
		{
			FString EnumToken = Token;
			if (!EnumToken.StartsWith(TEXT("functioninput_")))
			{
				EnumToken = FString::Printf(TEXT("FunctionInput_%s"), *Token);
			}
			const int64 Value = Enum->GetValueByName(FName(*EnumToken));
			if (Value != INDEX_NONE)
			{
				OutType = static_cast<EFunctionInputType>(Value);
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Unsupported function input type '%s'"), *Input);
		return false;
	}

	static TSharedPtr<FJsonObject> BuildMaterialFunctionInputJson(const UMaterialExpressionFunctionInput* InputExpression)
	{
		TSharedPtr<FJsonObject> InputObject = BuildNodeJson(InputExpression);
		if (!InputExpression || !InputObject.IsValid())
		{
			return InputObject;
		}

		InputObject->SetStringField(TEXT("input_name"), InputExpression->InputName.ToString());
		InputObject->SetStringField(TEXT("description"), InputExpression->Description);
		InputObject->SetNumberField(TEXT("sort_priority"), InputExpression->SortPriority);
		InputObject->SetStringField(TEXT("input_type"), FunctionInputTypeToString(InputExpression->InputType));
		InputObject->SetStringField(TEXT("input_type_display_name"), UMaterialExpressionFunctionInput::GetInputTypeDisplayName(InputExpression->InputType));
		InputObject->SetStringField(TEXT("material_value_type"), MaterialValueTypeToString(UMaterialExpressionFunctionInput::GetMaterialTypeFromInputType(InputExpression->InputType)));
		InputObject->SetBoolField(TEXT("use_preview_value_as_default"), InputExpression->bUsePreviewValueAsDefault != 0);
		InputObject->SetStringField(TEXT("id"), InputExpression->Id.IsValid() ? InputExpression->Id.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		InputObject->SetObjectField(TEXT("preview_value"), BuildColorJson(FLinearColor(
			static_cast<float>(InputExpression->PreviewValue.X),
			static_cast<float>(InputExpression->PreviewValue.Y),
			static_cast<float>(InputExpression->PreviewValue.Z),
			static_cast<float>(InputExpression->PreviewValue.W))));

		const bool bHasPreviewConnection = InputExpression->Preview.Expression != nullptr;
		InputObject->SetBoolField(TEXT("has_preview_connection"), bHasPreviewConnection);
		if (bHasPreviewConnection)
		{
			InputObject->SetStringField(TEXT("preview_node_id"), GetNodeId(InputExpression->Preview.Expression));
			InputObject->SetNumberField(TEXT("preview_output_index"), InputExpression->Preview.OutputIndex);
		}
		return InputObject;
	}

	static TSharedPtr<FJsonObject> BuildMaterialFunctionOutputJson(UMaterialExpressionFunctionOutput* OutputExpression)
	{
		TSharedPtr<FJsonObject> OutputObject = BuildNodeJson(OutputExpression);
		if (!OutputExpression || !OutputObject.IsValid())
		{
			return OutputObject;
		}

		OutputObject->SetStringField(TEXT("output_name"), OutputExpression->OutputName.ToString());
		OutputObject->SetStringField(TEXT("description"), OutputExpression->Description);
		OutputObject->SetNumberField(TEXT("sort_priority"), OutputExpression->SortPriority);
		OutputObject->SetStringField(TEXT("id"), OutputExpression->Id.IsValid() ? OutputExpression->Id.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		OutputObject->SetStringField(TEXT("inferred_value_type"), MaterialValueTypeToString(OutputExpression->GetOutputValueType(0)));

		const bool bConnected = OutputExpression->A.Expression != nullptr;
		OutputObject->SetBoolField(TEXT("is_connected"), bConnected);
		if (bConnected)
		{
			OutputObject->SetStringField(TEXT("linked_node_id"), GetNodeId(OutputExpression->A.Expression));
			OutputObject->SetStringField(TEXT("linked_node_name"), OutputExpression->A.Expression->GetName());
			OutputObject->SetNumberField(TEXT("linked_output_index"), OutputExpression->A.OutputIndex);
			OutputObject->SetStringField(TEXT("linked_output_name"), GetOutputPinDisplayName(OutputExpression->A.Expression, OutputExpression->A.OutputIndex, OutputExpression->A.Expression->GetOutput(OutputExpression->A.OutputIndex)));
		}
		return OutputObject;
	}

	static UMaterialExpressionFunctionInput* ResolveMaterialFunctionInputNode(
		UMaterialFunction* Function,
		const FString& NodeId,
		const FString& InputName,
		FString& OutError)
	{
		if (!Function)
		{
			OutError = TEXT("Material function is null");
			return nullptr;
		}

		const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
		const FString TrimmedInputName = InputName.TrimStartAndEnd();

		if (!TrimmedNodeId.IsEmpty())
		{
			FMaterialGraphContext Context;
			Context.MaterialFunction = Function;
			Context.AssetPath = NormalizeAssetPath(Function->GetPathName());
			UMaterialExpression* Node = FindNodeById(Context, TrimmedNodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId);
				return nullptr;
			}
			if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Node))
			{
				return FunctionInput;
			}
			OutError = FString::Printf(TEXT("Node '%s' is not a material function input"), *TrimmedNodeId);
			return nullptr;
		}

		if (TrimmedInputName.IsEmpty())
		{
			OutError = TEXT("Provide either 'node_id' or 'input_name'");
			return nullptr;
		}

		TArray<UMaterialExpressionFunctionInput*> Inputs;
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		GatherMaterialFunctionIONodes(Function, Inputs, Outputs);
		for (UMaterialExpressionFunctionInput* InputExpression : Inputs)
		{
			if (InputExpression && InputExpression->InputName.ToString().Equals(TrimmedInputName, ESearchCase::IgnoreCase))
			{
				return InputExpression;
			}
		}

		OutError = FString::Printf(TEXT("Function input not found: %s"), *TrimmedInputName);
		return nullptr;
	}

	static UMaterialExpressionFunctionOutput* ResolveMaterialFunctionOutputNode(
		UMaterialFunction* Function,
		const FString& NodeId,
		const FString& OutputName,
		FString& OutError)
	{
		if (!Function)
		{
			OutError = TEXT("Material function is null");
			return nullptr;
		}

		const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
		const FString TrimmedOutputName = OutputName.TrimStartAndEnd();

		if (!TrimmedNodeId.IsEmpty())
		{
			FMaterialGraphContext Context;
			Context.MaterialFunction = Function;
			Context.AssetPath = NormalizeAssetPath(Function->GetPathName());
			UMaterialExpression* Node = FindNodeById(Context, TrimmedNodeId);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId);
				return nullptr;
			}
			if (UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Node))
			{
				return FunctionOutput;
			}
			OutError = FString::Printf(TEXT("Node '%s' is not a material function output"), *TrimmedNodeId);
			return nullptr;
		}

		if (TrimmedOutputName.IsEmpty())
		{
			OutError = TEXT("Provide either 'node_id' or 'output_name'");
			return nullptr;
		}

		TArray<UMaterialExpressionFunctionInput*> Inputs;
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		GatherMaterialFunctionIONodes(Function, Inputs, Outputs);
		for (UMaterialExpressionFunctionOutput* OutputExpression : Outputs)
		{
			if (OutputExpression && OutputExpression->OutputName.ToString().Equals(TrimmedOutputName, ESearchCase::IgnoreCase))
			{
				return OutputExpression;
			}
		}

		OutError = FString::Printf(TEXT("Function output not found: %s"), *TrimmedOutputName);
		return nullptr;
	}

	enum class ECollectionParameterKind : uint8
	{
		Unknown,
		Scalar,
		Vector
	};

	static FString CollectionParameterKindToString(const ECollectionParameterKind Kind)
	{
		switch (Kind)
		{
		case ECollectionParameterKind::Scalar:
			return TEXT("scalar");
		case ECollectionParameterKind::Vector:
			return TEXT("vector");
		default:
			return TEXT("unknown");
		}
	}

	static bool ParseOptionalCollectionParameterKind(
		const TSharedPtr<FJsonObject>& Params,
		ECollectionParameterKind& OutKind,
		bool& bOutHasKind,
		FString& OutError)
	{
		OutKind = ECollectionParameterKind::Unknown;
		bOutHasKind = false;
		if (!Params.IsValid())
		{
			return true;
		}

		FString KindString;
		if (!Params->TryGetStringField(TEXT("parameter_type"), KindString) &&
			!Params->TryGetStringField(TEXT("type"), KindString))
		{
			return true;
		}

		const FString Token = NormalizeParameterToken(KindString);
		bOutHasKind = true;
		if (Token == TEXT("scalar") || Token == TEXT("float") || Token == TEXT("float1"))
		{
			OutKind = ECollectionParameterKind::Scalar;
			return true;
		}
		if (Token == TEXT("vector") || Token == TEXT("float4") || Token == TEXT("color"))
		{
			OutKind = ECollectionParameterKind::Vector;
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported parameter_type '%s' (expected scalar or vector)"), *KindString);
		return false;
	}

	static UMaterialParameterCollection* ResolveMaterialParameterCollectionAsset(const FString& InputPath, FString& OutAssetPath, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid parameter collection path: %s"), *InputPath);
			return nullptr;
		}

		UMaterialParameterCollection* Collection = LoadAssetAs<UMaterialParameterCollection>(OutAssetPath);
		if (!Collection)
		{
			OutError = FString::Printf(TEXT("Material parameter collection not found: %s"), *OutAssetPath);
			return nullptr;
		}

		return Collection;
	}

	static int32 FindCollectionScalarParameterIndex(const UMaterialParameterCollection* Collection, const FString& ParameterName)
	{
		if (!Collection)
		{
			return INDEX_NONE;
		}

		for (int32 Index = 0; Index < Collection->ScalarParameters.Num(); ++Index)
		{
			if (Collection->ScalarParameters[Index].ParameterName.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static int32 FindCollectionVectorParameterIndex(const UMaterialParameterCollection* Collection, const FString& ParameterName)
	{
		if (!Collection)
		{
			return INDEX_NONE;
		}

		for (int32 Index = 0; Index < Collection->VectorParameters.Num(); ++Index)
		{
			if (Collection->VectorParameters[Index].ParameterName.ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static TSharedPtr<FJsonObject> BuildCollectionScalarParameterJson(
		const FCollectionScalarParameter& Parameter,
		const FString& SourceCollectionPath = FString(),
		const bool bInherited = false)
	{
		TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
		ParameterObject->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		ParameterObject->SetStringField(TEXT("parameter_name"), Parameter.ParameterName.ToString());
		ParameterObject->SetNumberField(TEXT("default_value"), Parameter.DefaultValue);
		ParameterObject->SetStringField(TEXT("id"), Parameter.Id.IsValid() ? Parameter.Id.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		if (!SourceCollectionPath.IsEmpty())
		{
			ParameterObject->SetStringField(TEXT("source_collection_path"), SourceCollectionPath);
		}
		ParameterObject->SetBoolField(TEXT("is_inherited"), bInherited);
		return ParameterObject;
	}

	static TSharedPtr<FJsonObject> BuildCollectionVectorParameterJson(
		const FCollectionVectorParameter& Parameter,
		const FString& SourceCollectionPath = FString(),
		const bool bInherited = false)
	{
		TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
		ParameterObject->SetStringField(TEXT("parameter_type"), TEXT("vector"));
		ParameterObject->SetStringField(TEXT("parameter_name"), Parameter.ParameterName.ToString());
		ParameterObject->SetObjectField(TEXT("default_value"), BuildColorJson(Parameter.DefaultValue));
		ParameterObject->SetStringField(TEXT("id"), Parameter.Id.IsValid() ? Parameter.Id.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		if (!SourceCollectionPath.IsEmpty())
		{
			ParameterObject->SetStringField(TEXT("source_collection_path"), SourceCollectionPath);
		}
		ParameterObject->SetBoolField(TEXT("is_inherited"), bInherited);
		return ParameterObject;
	}

	static void GatherCollectionHierarchy(const UMaterialParameterCollection* Collection, TArray<const UMaterialParameterCollection*>& OutHierarchy)
	{
		OutHierarchy.Reset();
		if (!Collection)
		{
			return;
		}

		const UMaterialParameterCollection* Current = Collection;
		while (Current)
		{
			OutHierarchy.Insert(Current, 0);
			Current = Current->GetBaseParameterCollection();
		}
	}

	enum class EMaterialSymbolType : uint8
	{
		Unknown,
		Parameter,
		FunctionCall,
		Node
	};

	static FString MaterialSymbolTypeToString(const EMaterialSymbolType Type)
	{
		switch (Type)
		{
		case EMaterialSymbolType::Parameter:
			return TEXT("parameter");
		case EMaterialSymbolType::FunctionCall:
			return TEXT("function_call");
		case EMaterialSymbolType::Node:
			return TEXT("node");
		default:
			return TEXT("unknown");
		}
	}

	static bool ParseMaterialSymbolType(const FString& SymbolTypeString, EMaterialSymbolType& OutType)
	{
		const FString Normalized = NormalizeParameterToken(SymbolTypeString);
		if (Normalized == TEXT("parameter") || Normalized == TEXT("param"))
		{
			OutType = EMaterialSymbolType::Parameter;
			return true;
		}
		if (Normalized == TEXT("function_call") || Normalized == TEXT("functioncall") || Normalized == TEXT("function"))
		{
			OutType = EMaterialSymbolType::FunctionCall;
			return true;
		}
		if (Normalized == TEXT("node"))
		{
			OutType = EMaterialSymbolType::Node;
			return true;
		}

		OutType = EMaterialSymbolType::Unknown;
		return false;
	}

	static FString MaterialMessageSeverityToString(const EMessageSeverity::Type Severity)
	{
		switch (Severity)
		{
		case EMessageSeverity::Error:
			return TEXT("error");
		case EMessageSeverity::Warning:
			return TEXT("warning");
		case EMessageSeverity::PerformanceWarning:
			return TEXT("performance_warning");
		case EMessageSeverity::Info:
			return TEXT("info");
		default:
			return TEXT("unknown");
		}
	}

	static FString MaterialDataValidationResultToString(const EDataValidationResult Result)
	{
		switch (Result)
		{
		case EDataValidationResult::Valid:
			return TEXT("valid");
		case EDataValidationResult::Invalid:
			return TEXT("invalid");
		case EDataValidationResult::NotValidated:
			return TEXT("not_validated");
		default:
			return TEXT("not_validated");
		}
	}

	struct FMaterialCompileDiagnostics
	{
		bool bIsCompiling = false;
		bool bHadCompileError = false;
		TArray<FString> CompileErrors;
		TArray<TWeakObjectPtr<UMaterialExpression>> ErrorExpressions;

		void Reset()
		{
			bIsCompiling = false;
			bHadCompileError = false;
			CompileErrors.Reset();
			ErrorExpressions.Reset();
		}
	};

	static void GatherMaterialCompileDiagnostics(UMaterial* Material, FMaterialCompileDiagnostics& OutDiagnostics)
	{
		OutDiagnostics.Reset();
		if (!Material)
		{
			return;
		}

		OutDiagnostics.bIsCompiling = Material->IsCompiling();

		TSet<FString> SeenErrors;
		TSet<const UMaterialExpression*> SeenErrorExpressions;

		const ERHIFeatureLevel::Type FeatureLevels[] =
		{
			ERHIFeatureLevel::SM6,
			ERHIFeatureLevel::SM5,
			ERHIFeatureLevel::ES3_1
		};

		for (const ERHIFeatureLevel::Type FeatureLevel : FeatureLevels)
		{
			OutDiagnostics.bHadCompileError = OutDiagnostics.bHadCompileError || Material->IsCompilingOrHadCompileError(FeatureLevel);

			const FMaterialResource* MaterialResource = Material->GetMaterialResource(FeatureLevel);
			if (!MaterialResource)
			{
				continue;
			}

			for (const FString& CompileError : MaterialResource->GetCompileErrors())
			{
				const FString TrimmedError = CompileError.TrimStartAndEnd();
				if (!TrimmedError.IsEmpty() && !SeenErrors.Contains(TrimmedError))
				{
					SeenErrors.Add(TrimmedError);
					OutDiagnostics.CompileErrors.Add(TrimmedError);
				}
			}

			for (UMaterialExpression* ErrorExpression : MaterialResource->GetErrorExpressions())
			{
				if (!ErrorExpression || SeenErrorExpressions.Contains(ErrorExpression))
				{
					continue;
				}
				SeenErrorExpressions.Add(ErrorExpression);
				OutDiagnostics.ErrorExpressions.Add(ErrorExpression);
			}
		}

		OutDiagnostics.bHadCompileError = OutDiagnostics.bHadCompileError || OutDiagnostics.CompileErrors.Num() > 0;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildMaterialCompileMessagesJson(const FMaterialCompileDiagnostics& Diagnostics, int32 MaxMessages = INDEX_NONE)
	{
		const int32 MessageLimit = MaxMessages >= 0
			? FMath::Min(MaxMessages, Diagnostics.CompileErrors.Num())
			: Diagnostics.CompileErrors.Num();

		TArray<TSharedPtr<FJsonValue>> MessagesJson;
		MessagesJson.Reserve(MessageLimit);

		for (int32 MessageIndex = 0; MessageIndex < MessageLimit; ++MessageIndex)
		{
			TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
			MessageObj->SetStringField(TEXT("severity"), TEXT("error"));
			MessageObj->SetNumberField(TEXT("severity_code"), static_cast<int32>(EMessageSeverity::Error));
			MessageObj->SetStringField(TEXT("text"), Diagnostics.CompileErrors[MessageIndex]);

			const UMaterialExpression* ErrorExpression = Diagnostics.ErrorExpressions.IsValidIndex(MessageIndex)
				? Diagnostics.ErrorExpressions[MessageIndex].Get()
				: nullptr;
			MessageObj->SetBoolField(TEXT("has_node_context"), ErrorExpression != nullptr);
			if (ErrorExpression)
			{
				MessageObj->SetObjectField(TEXT("node"), BuildNodeJson(ErrorExpression));
			}

			MessagesJson.Add(MakeShared<FJsonValueObject>(MessageObj));
		}

		return MessagesJson;
	}

	static TSharedPtr<FJsonObject> BuildMaterialValidationIssueJson(const FDataValidationContext::FIssue& Issue)
	{
		TSharedPtr<FJsonObject> IssueObj = MakeShared<FJsonObject>();
		IssueObj->SetStringField(TEXT("severity"), MaterialMessageSeverityToString(Issue.Severity));
		IssueObj->SetNumberField(TEXT("severity_code"), static_cast<int32>(Issue.Severity));
		IssueObj->SetStringField(TEXT("text"), Issue.Message.ToString());
		return IssueObj;
	}

	static UObject* ResolveMaterialManagedAsset(
		const FString& InputPath,
		FString& OutAssetPath,
		FString& OutAssetKind,
		FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		OutAssetKind.Reset();
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid asset path: %s"), *InputPath);
			return nullptr;
		}

		if (UMaterial* Material = LoadAssetAs<UMaterial>(OutAssetPath))
		{
			OutAssetKind = TEXT("material");
			return Material;
		}
		if (UMaterialFunction* Function = LoadAssetAs<UMaterialFunction>(OutAssetPath))
		{
			OutAssetKind = TEXT("material_function");
			return Function;
		}
		if (UMaterialInstanceConstant* Instance = LoadAssetAs<UMaterialInstanceConstant>(OutAssetPath))
		{
			OutAssetKind = TEXT("material_instance");
			return Instance;
		}
		if (UMaterialParameterCollection* Collection = LoadAssetAs<UMaterialParameterCollection>(OutAssetPath))
		{
			OutAssetKind = TEXT("material_parameter_collection");
			return Collection;
		}

		OutError = FString::Printf(TEXT("Material asset not found or unsupported asset type: %s"), *OutAssetPath);
		return nullptr;
	}

	static UMaterialInterface* ResolveMaterialInterfaceAsset(
		const FString& InputPath,
		FString& OutAssetPath,
		FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(InputPath);
		if (!FPackageName::IsValidLongPackageName(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid asset path: %s"), *InputPath);
			return nullptr;
		}

		UMaterialInterface* MaterialInterface = LoadAssetAs<UMaterialInterface>(OutAssetPath);
		if (!MaterialInterface)
		{
			OutError = FString::Printf(TEXT("Material or material instance not found: %s"), *OutAssetPath);
		}
		return MaterialInterface;
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
	AddTool(TEXT("list_parameters"), TEXT("List parameter expressions in a material graph."));
	AddTool(TEXT("add_parameter"), TEXT("Add a parameter expression node to a material graph."));
	AddTool(TEXT("remove_parameter"), TEXT("Remove a parameter expression node from a material graph."));
	AddTool(TEXT("rename_parameter"), TEXT("Rename a parameter in a material graph."));
	AddTool(TEXT("set_parameter_default"), TEXT("Set the default value on a parameter expression."));
	AddTool(TEXT("set_parameter_metadata"), TEXT("Set parameter metadata (group, sort priority, description)."));
	AddTool(TEXT("set_parameter_channel_names"), TEXT("Set channel display names for vector/texture parameters."));
	AddTool(TEXT("material_instance/set_parent"), TEXT("Set the parent material or material instance for a material instance."));
	AddTool(TEXT("material_instance/list_overrides"), TEXT("List parameter overrides on a material instance."));
	AddTool(TEXT("material_instance/clear_override"), TEXT("Clear one parameter override on a material instance."));
	AddTool(TEXT("material_instance/set_scalar"), TEXT("Set a scalar parameter override on a material instance."));
	AddTool(TEXT("material_instance/set_vector"), TEXT("Set a vector parameter override on a material instance."));
	AddTool(TEXT("material_instance/set_texture"), TEXT("Set a texture parameter override on a material instance."));
	AddTool(TEXT("material_instance/set_static_switch"), TEXT("Set a static switch parameter override on a material instance."));
	AddTool(TEXT("material_instance/set_static_component_mask"), TEXT("Set a static component mask override on a material instance."));
	AddTool(TEXT("material_instance/copy_overrides_from_instance"), TEXT("Copy overrides from one material instance to another."));
	AddTool(TEXT("material_function/get_info"), TEXT("Get metadata and IO summary for a material function."));
	AddTool(TEXT("material_function/list_inputs"), TEXT("List input nodes in a material function."));
	AddTool(TEXT("material_function/list_outputs"), TEXT("List output nodes in a material function."));
	AddTool(TEXT("material_function/create_input"), TEXT("Create a function input node in a material function."));
	AddTool(TEXT("material_function/create_output"), TEXT("Create a function output node in a material function."));
	AddTool(TEXT("material_function/remove_input"), TEXT("Remove a function input node from a material function."));
	AddTool(TEXT("material_function/remove_output"), TEXT("Remove a function output node from a material function."));
	AddTool(TEXT("material_function/add_call_node"), TEXT("Add a material function call node to a material or material function graph."));
	AddTool(TEXT("material_function/set_io_types"), TEXT("Set input/output value typing for a material function interface."));
	AddTool(TEXT("material_function/compile"), TEXT("Compile/update a material function and dependent materials."));
	AddTool(TEXT("material_collection/get_info"), TEXT("Get metadata and parameter counts for a material parameter collection."));
	AddTool(TEXT("material_collection/list_parameters"), TEXT("List scalar/vector parameters in a material parameter collection."));
	AddTool(TEXT("material_collection/add_scalar"), TEXT("Add a scalar parameter to a material parameter collection."));
	AddTool(TEXT("material_collection/add_vector"), TEXT("Add a vector parameter to a material parameter collection."));
	AddTool(TEXT("material_collection/remove_parameter"), TEXT("Remove a scalar or vector parameter from a material parameter collection."));
	AddTool(TEXT("material_collection/rename_parameter"), TEXT("Rename a scalar or vector parameter in a material parameter collection."));
	AddTool(TEXT("material_collection/set_default_value"), TEXT("Set a scalar or vector default value in a material parameter collection."));
	AddTool(TEXT("find_references"), TEXT("Find references for a material symbol (parameter, function call, or node)."));
	AddTool(TEXT("rename_symbol"), TEXT("Rename a material symbol safely (parameter, function call, or node)."));
	AddTool(TEXT("replace_function_calls"), TEXT("Replace material function call nodes across a graph."));
	AddTool(TEXT("remove_unused_parameters"), TEXT("Remove unreferenced parameter expressions from a material graph."));
	AddTool(TEXT("remove_orphan_nodes"), TEXT("Remove orphaned nodes that do not contribute to final outputs."));
	AddTool(TEXT("compile_material"), TEXT("Compile/recompile a material and return diagnostics."));
	AddTool(TEXT("get_compile_result"), TEXT("Get compile diagnostics for a material, with optional compile."));
	AddTool(TEXT("validate_material"), TEXT("Run data validation checks for a material-related asset."));
	AddTool(TEXT("get_material_status"), TEXT("Get compile/dirty/validation status for a material-related asset."));
	AddTool(TEXT("list_material_warnings"), TEXT("List warning-level diagnostics for a material-related asset."));
	AddTool(TEXT("get_shader_stats"), TEXT("Get shader instruction and sampler stats for a material or material instance."));
	AddTool(TEXT("begin_transaction"), TEXT("Begin a managed editor transaction for material authoring operations."));
	AddTool(TEXT("end_transaction"), TEXT("End an active managed material transaction."));
	AddTool(TEXT("cancel_transaction"), TEXT("Cancel and rollback an active managed material transaction."));
	AddTool(TEXT("dry_run_validate"), TEXT("Run non-mutating validation checks for a material-related asset."));
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
	if (MethodName == TEXT("list_parameters")) return HandleListParameters(Request);
	if (MethodName == TEXT("add_parameter")) return HandleAddParameter(Request);
	if (MethodName == TEXT("remove_parameter")) return HandleRemoveParameter(Request);
	if (MethodName == TEXT("rename_parameter")) return HandleRenameParameter(Request);
	if (MethodName == TEXT("set_parameter_default")) return HandleSetParameterDefault(Request);
	if (MethodName == TEXT("set_parameter_metadata")) return HandleSetParameterMetadata(Request);
	if (MethodName == TEXT("set_parameter_channel_names")) return HandleSetParameterChannelNames(Request);
	if (MethodName == TEXT("material_instance/set_parent")) return HandleMaterialInstanceSetParent(Request);
	if (MethodName == TEXT("material_instance/list_overrides")) return HandleMaterialInstanceListOverrides(Request);
	if (MethodName == TEXT("material_instance/clear_override")) return HandleMaterialInstanceClearOverride(Request);
	if (MethodName == TEXT("material_instance/set_scalar")) return HandleMaterialInstanceSetScalar(Request);
	if (MethodName == TEXT("material_instance/set_vector")) return HandleMaterialInstanceSetVector(Request);
	if (MethodName == TEXT("material_instance/set_texture")) return HandleMaterialInstanceSetTexture(Request);
	if (MethodName == TEXT("material_instance/set_static_switch")) return HandleMaterialInstanceSetStaticSwitch(Request);
	if (MethodName == TEXT("material_instance/set_static_component_mask")) return HandleMaterialInstanceSetStaticComponentMask(Request);
	if (MethodName == TEXT("material_instance/copy_overrides_from_instance")) return HandleMaterialInstanceCopyOverridesFromInstance(Request);
	if (MethodName == TEXT("material_function/get_info")) return HandleMaterialFunctionGetInfo(Request);
	if (MethodName == TEXT("material_function/list_inputs")) return HandleMaterialFunctionListInputs(Request);
	if (MethodName == TEXT("material_function/list_outputs")) return HandleMaterialFunctionListOutputs(Request);
	if (MethodName == TEXT("material_function/create_input")) return HandleMaterialFunctionCreateInput(Request);
	if (MethodName == TEXT("material_function/create_output")) return HandleMaterialFunctionCreateOutput(Request);
	if (MethodName == TEXT("material_function/remove_input")) return HandleMaterialFunctionRemoveInput(Request);
	if (MethodName == TEXT("material_function/remove_output")) return HandleMaterialFunctionRemoveOutput(Request);
	if (MethodName == TEXT("material_function/add_call_node")) return HandleMaterialFunctionAddCallNode(Request);
	if (MethodName == TEXT("material_function/set_io_types")) return HandleMaterialFunctionSetIOTypes(Request);
	if (MethodName == TEXT("material_function/compile")) return HandleMaterialFunctionCompile(Request);
	if (MethodName == TEXT("material_collection/get_info")) return HandleMaterialCollectionGetInfo(Request);
	if (MethodName == TEXT("material_collection/list_parameters")) return HandleMaterialCollectionListParameters(Request);
	if (MethodName == TEXT("material_collection/add_scalar")) return HandleMaterialCollectionAddScalar(Request);
	if (MethodName == TEXT("material_collection/add_vector")) return HandleMaterialCollectionAddVector(Request);
	if (MethodName == TEXT("material_collection/remove_parameter")) return HandleMaterialCollectionRemoveParameter(Request);
	if (MethodName == TEXT("material_collection/rename_parameter")) return HandleMaterialCollectionRenameParameter(Request);
	if (MethodName == TEXT("material_collection/set_default_value")) return HandleMaterialCollectionSetDefaultValue(Request);
	if (MethodName == TEXT("find_references")) return HandleFindReferences(Request);
	if (MethodName == TEXT("rename_symbol")) return HandleRenameSymbol(Request);
	if (MethodName == TEXT("replace_function_calls")) return HandleReplaceFunctionCalls(Request);
	if (MethodName == TEXT("remove_unused_parameters")) return HandleRemoveUnusedParameters(Request);
	if (MethodName == TEXT("remove_orphan_nodes")) return HandleRemoveOrphanNodes(Request);
	if (MethodName == TEXT("compile_material")) return HandleCompileMaterial(Request);
	if (MethodName == TEXT("get_compile_result")) return HandleGetCompileResult(Request);
	if (MethodName == TEXT("validate_material")) return HandleValidateMaterial(Request);
	if (MethodName == TEXT("get_material_status")) return HandleGetMaterialStatus(Request);
	if (MethodName == TEXT("list_material_warnings")) return HandleListMaterialWarnings(Request);
	if (MethodName == TEXT("get_shader_stats")) return HandleGetShaderStats(Request);
	if (MethodName == TEXT("begin_transaction")) return HandleBeginTransaction(Request);
	if (MethodName == TEXT("end_transaction")) return HandleEndTransaction(Request);
	if (MethodName == TEXT("cancel_transaction")) return HandleCancelTransaction(Request);
	if (MethodName == TEXT("dry_run_validate")) return HandleDryRunValidate(Request);
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

FMCPResponse FMaterialService::HandleListParameters(const FMCPRequest& Request)
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

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	auto Task = [AssetPath, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/list_parameters only supports UMaterial assets"));
		}

		TArray<UMaterialExpression*> Parameters;
		GatherParameterExpressions(Context, Parameters);

		TArray<TSharedPtr<FJsonValue>> ParameterArray;
		for (UMaterialExpression* Parameter : Parameters)
		{
			if (!Parameter)
			{
				continue;
			}

			if (bHasTypeFilter && GetParameterNodeType(Parameter) != TypeFilter)
			{
				continue;
			}

			ParameterArray.Add(MakeShared<FJsonValueObject>(BuildParameterJson(Parameter)));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetArrayField(TEXT("parameters"), ParameterArray);
		Result->SetNumberField(TEXT("parameter_count"), ParameterArray.Num());
		if (bHasTypeFilter)
		{
			Result->SetStringField(TEXT("parameter_type_filter"), ParameterTypeToString(TypeFilter));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleAddParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString ParameterTypeString;
	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_type"), ParameterTypeString))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_type'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	EMaterialParameterNodeType ParameterType = EMaterialParameterNodeType::Unknown;
	if (!ParseParameterTypeToken(ParameterTypeString, ParameterType))
	{
		return InvalidParams(Request.Id, FString::Printf(TEXT("Unsupported parameter_type '%s'"), *ParameterTypeString));
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

	FString GroupName;
	const bool bHasGroup = Request.Params->TryGetStringField(TEXT("group"), GroupName);

	bool bHasSortPriority = false;
	int32 SortPriority = 0;
	double SortPriorityDouble = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("sort_priority"), SortPriorityDouble))
	{
		bHasSortPriority = true;
		SortPriority = static_cast<int32>(SortPriorityDouble);
	}

	FString Description;
	const bool bHasDescription = Request.Params->TryGetStringField(TEXT("description"), Description);

	const TSharedPtr<FJsonObject> Params = Request.Params;
	auto Task = [AssetPath, ParameterType, TrimmedParameterName, NodePosX, NodePosY, bHasGroup, GroupName, bHasSortPriority, SortPriority, bHasDescription, Description, Params]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/add_parameter only supports UMaterial assets"));
		}

		FString ConflictingNodeId;
		if (DoesParameterNameConflict(Context, nullptr, FName(*TrimmedParameterName), ConflictingNodeId))
		{
			return MakeFailure(FString::Printf(TEXT("Parameter name already exists: %s (node_id=%s)"), *TrimmedParameterName, *ConflictingNodeId));
		}

		UClass* ExpressionClass = nullptr;
		switch (ParameterType)
		{
		case EMaterialParameterNodeType::Scalar:
			ExpressionClass = UMaterialExpressionScalarParameter::StaticClass();
			break;
		case EMaterialParameterNodeType::Vector:
			ExpressionClass = UMaterialExpressionVectorParameter::StaticClass();
			break;
		case EMaterialParameterNodeType::Texture:
			ExpressionClass = UMaterialExpressionTextureSampleParameter2D::StaticClass();
			break;
		case EMaterialParameterNodeType::StaticSwitch:
			ExpressionClass = UMaterialExpressionStaticSwitchParameter::StaticClass();
			break;
		case EMaterialParameterNodeType::StaticComponentMask:
			ExpressionClass = UMaterialExpressionStaticComponentMaskParameter::StaticClass();
			break;
		case EMaterialParameterNodeType::RuntimeVirtualTexture:
			ExpressionClass = UMaterialExpressionRuntimeVirtualTextureSampleParameter::StaticClass();
			break;
		case EMaterialParameterNodeType::Font:
			ExpressionClass = UMaterialExpressionFontSampleParameter::StaticClass();
			break;
		default:
			break;
		}

		if (!ExpressionClass)
		{
			return MakeFailure(TEXT("Failed to resolve expression class for requested parameter_type"));
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Context.Material,
			nullptr,
			ExpressionClass,
			nullptr,
			NodePosX,
			NodePosY,
			true);
		if (!NewExpression)
		{
			return MakeFailure(TEXT("Failed to create parameter expression"));
		}

		NewExpression->Modify();
		if (!SetParameterExpressionName(NewExpression, FName(*TrimmedParameterName)))
		{
			return MakeFailure(TEXT("Created expression does not expose a parameter name"));
		}
		if (bHasGroup)
		{
			SetParameterExpressionGroup(NewExpression, GroupName.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*GroupName.TrimStartAndEnd()));
		}
		if (bHasSortPriority)
		{
			SetParameterExpressionSortPriority(NewExpression, SortPriority);
		}
		if (bHasDescription)
		{
			NewExpression->Desc = Description;
		}

		FGuid ParameterGuid = GetParameterExpressionGuid(NewExpression);
		if (!ParameterGuid.IsValid())
		{
			ParameterGuid = FGuid::NewGuid();
			SetParameterExpressionGuid(NewExpression, ParameterGuid);
		}

		switch (ParameterType)
		{
		case EMaterialParameterNodeType::Scalar:
			if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(NewExpression))
			{
				double Value = 0.0;
				if (Params->TryGetNumberField(TEXT("default_value"), Value))
				{
					Scalar->DefaultValue = static_cast<float>(Value);
				}
			}
			break;
		case EMaterialParameterNodeType::Vector:
			if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(NewExpression))
			{
				if (Params->HasField(TEXT("default_value")) || Params->HasField(TEXT("default_r")) || Params->HasField(TEXT("default_g")) || Params->HasField(TEXT("default_b")) || Params->HasField(TEXT("default_a")))
				{
					FLinearColor DefaultColor = Vector->DefaultValue;
					if (!TryReadLinearColor(Params, TEXT("default_value"), DefaultColor, Error))
					{
						return MakeFailure(Error);
					}
					Vector->DefaultValue = DefaultColor;
				}

				bool bHasChannelNameFields = false;
				TryApplyChannelNameFields(Params, Vector->ChannelNames, bHasChannelNameFields);
			}
			break;
		case EMaterialParameterNodeType::Texture:
			if (UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(NewExpression))
			{
				FString TexturePath;
				if (Params->TryGetStringField(TEXT("texture_path"), TexturePath) || Params->TryGetStringField(TEXT("default_texture_path"), TexturePath))
				{
					TexturePath = TexturePath.TrimStartAndEnd();
					Texture->Texture = TexturePath.IsEmpty() ? nullptr : LoadAssetAs<UTexture>(TexturePath);
					if (!TexturePath.IsEmpty() && !Texture->Texture)
					{
						return MakeFailure(FString::Printf(TEXT("Texture asset not found: %s"), *TexturePath));
					}
				}

				bool bHasChannelNameFields = false;
				TryApplyChannelNameFields(Params, Texture->ChannelNames, bHasChannelNameFields);
				if (bHasChannelNameFields)
				{
					Texture->ApplyChannelNames();
				}
			}
			break;
		case EMaterialParameterNodeType::StaticSwitch:
			if (UMaterialExpressionStaticSwitchParameter* StaticSwitch = Cast<UMaterialExpressionStaticSwitchParameter>(NewExpression))
			{
				bool bDefaultValue = false;
				if (Params->TryGetBoolField(TEXT("default_value"), bDefaultValue))
				{
					StaticSwitch->DefaultValue = bDefaultValue ? 1 : 0;
				}
			}
			break;
		case EMaterialParameterNodeType::StaticComponentMask:
			if (UMaterialExpressionStaticComponentMaskParameter* StaticMask = Cast<UMaterialExpressionStaticComponentMaskParameter>(NewExpression))
			{
				if (Params->HasField(TEXT("default_mask")) || Params->HasField(TEXT("default_value")) || Params->HasField(TEXT("default_r")) || Params->HasField(TEXT("default_g")) || Params->HasField(TEXT("default_b")) || Params->HasField(TEXT("default_a")))
				{
					bool bR = false;
					bool bG = false;
					bool bB = false;
					bool bA = false;
					if (!TryReadStaticComponentMaskDefaults(Params, bR, bG, bB, bA, Error))
					{
						return MakeFailure(Error);
					}
					StaticMask->DefaultR = bR ? 1 : 0;
					StaticMask->DefaultG = bG ? 1 : 0;
					StaticMask->DefaultB = bB ? 1 : 0;
					StaticMask->DefaultA = bA ? 1 : 0;
				}
			}
			break;
		case EMaterialParameterNodeType::RuntimeVirtualTexture:
			if (UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTexture = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(NewExpression))
			{
				FString RuntimeVirtualTexturePath;
				if (Params->TryGetStringField(TEXT("runtime_virtual_texture_path"), RuntimeVirtualTexturePath) || Params->TryGetStringField(TEXT("virtual_texture_path"), RuntimeVirtualTexturePath))
				{
					RuntimeVirtualTexturePath = RuntimeVirtualTexturePath.TrimStartAndEnd();
					RuntimeVirtualTexture->VirtualTexture = RuntimeVirtualTexturePath.IsEmpty() ? nullptr : LoadAssetAs<URuntimeVirtualTexture>(RuntimeVirtualTexturePath);
					if (!RuntimeVirtualTexturePath.IsEmpty() && !RuntimeVirtualTexture->VirtualTexture)
					{
						return MakeFailure(FString::Printf(TEXT("Runtime virtual texture asset not found: %s"), *RuntimeVirtualTexturePath));
					}
				}
			}
			break;
		case EMaterialParameterNodeType::Font:
			if (UMaterialExpressionFontSampleParameter* Font = Cast<UMaterialExpressionFontSampleParameter>(NewExpression))
			{
				FString FontPath;
				if (Params->TryGetStringField(TEXT("font_path"), FontPath) || Params->TryGetStringField(TEXT("default_font_path"), FontPath))
				{
					FontPath = FontPath.TrimStartAndEnd();
					Font->Font = FontPath.IsEmpty() ? nullptr : LoadAssetAs<UFont>(FontPath);
					if (!FontPath.IsEmpty() && !Font->Font)
					{
						return MakeFailure(FString::Printf(TEXT("Font asset not found: %s"), *FontPath));
					}
				}

				double FontPageDouble = 0.0;
				if (Params->TryGetNumberField(TEXT("font_page"), FontPageDouble))
				{
					Font->FontTexturePage = static_cast<int32>(FontPageDouble);
				}
			}
			break;
		default:
			break;
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("parameter_type"), ParameterTypeToString(ParameterType));
		Result->SetObjectField(TEXT("parameter"), BuildParameterJson(NewExpression));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRemoveParameter(const FMCPRequest& Request)
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

	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString ParameterName;
	Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName);
	bool bRemoveAllMatches = false;
	Request.Params->TryGetBoolField(TEXT("remove_all_matches"), bRemoveAllMatches);

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	auto Task = [AssetPath, NodeId, ParameterName, bRemoveAllMatches, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/remove_parameter only supports UMaterial assets"));
		}

		TArray<UMaterialExpression*> Targets;
		if (!NodeId.TrimStartAndEnd().IsEmpty())
		{
			UMaterialExpression* Parameter = ResolveParameterExpression(Context, NodeId, FString(), bHasTypeFilter, TypeFilter, Error);
			if (!Parameter)
			{
				return MakeFailure(Error);
			}
			Targets.Add(Parameter);
		}
		else
		{
			GatherParameterMatchesByName(Context, ParameterName, bHasTypeFilter, TypeFilter, Targets);
			if (Targets.Num() == 0)
			{
				return MakeFailure(ParameterName.TrimStartAndEnd().IsEmpty()
					? TEXT("Missing target parameter: provide node_id or parameter_name")
					: FString::Printf(TEXT("Parameter not found: %s"), *ParameterName));
			}
			if (Targets.Num() > 1 && !bRemoveAllMatches)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter name is ambiguous (%d matches). Provide node_id or set remove_all_matches=true."), Targets.Num()));
			}
			if (!bRemoveAllMatches && Targets.Num() > 1)
			{
				Targets.SetNum(1);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Removed;
		for (UMaterialExpression* Target : Targets)
		{
			if (!Target)
			{
				continue;
			}

			Removed.Add(MakeShared<FJsonValueObject>(BuildParameterJson(Target)));
			UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, Target);
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetArrayField(TEXT("removed_parameters"), Removed);
		Result->SetNumberField(TEXT("removed_count"), Removed.Num());
		Result->SetBoolField(TEXT("remove_all_matches"), bRemoveAllMatches);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRenameParameter(const FMCPRequest& Request)
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

	FString NewParameterName;
	if (!Request.Params->TryGetStringField(TEXT("new_parameter_name"), NewParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_parameter_name'"));
	}
	NewParameterName = NewParameterName.TrimStartAndEnd();
	if (NewParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'new_parameter_name' cannot be empty"));
	}

	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString ParameterName;
	Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName);

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	auto Task = [AssetPath, NodeId, ParameterName, NewParameterName, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/rename_parameter only supports UMaterial assets"));
		}

		UMaterialExpression* Target = ResolveParameterExpression(Context, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		const FName OldName = GetParameterExpressionName(Target);
		const FName NewName = FName(*NewParameterName);
		if (OldName.IsEqual(NewName, ENameCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
			Result->SetBoolField(TEXT("changed"), false);
			Result->SetStringField(TEXT("old_parameter_name"), OldName.ToString());
			Result->SetStringField(TEXT("new_parameter_name"), NewName.ToString());
			Result->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
			return Result;
		}

		FString ConflictingNodeId;
		if (DoesParameterNameConflict(Context, Target, NewName, ConflictingNodeId))
		{
			return MakeFailure(FString::Printf(TEXT("Parameter name already exists: %s (node_id=%s)"), *NewName.ToString(), *ConflictingNodeId));
		}

		Target->Modify();
		if (!SetParameterExpressionName(Target, NewName))
		{
			return MakeFailure(TEXT("Target node does not expose a parameter name"));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetStringField(TEXT("old_parameter_name"), OldName.ToString());
		Result->SetStringField(TEXT("new_parameter_name"), NewName.ToString());
		Result->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetParameterDefault(const FMCPRequest& Request)
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

	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString ParameterName;
	Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName);

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	const TSharedPtr<FJsonObject> Params = Request.Params;
	auto Task = [AssetPath, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Params]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_parameter_default only supports UMaterial assets"));
		}

		UMaterialExpression* Target = ResolveParameterExpression(Context, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		const EMaterialParameterNodeType TargetType = GetParameterNodeType(Target);
		Target->Modify();

		switch (TargetType)
		{
		case EMaterialParameterNodeType::Scalar:
			{
				UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Target);
				double Value = 0.0;
				if (!Scalar || !Params->TryGetNumberField(TEXT("default_value"), Value))
				{
					return MakeFailure(TEXT("Scalar parameter requires numeric field 'default_value'"));
				}
				Scalar->DefaultValue = static_cast<float>(Value);
			}
			break;
		case EMaterialParameterNodeType::Vector:
			{
				UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Target);
				if (!Vector)
				{
					return MakeFailure(TEXT("Target parameter is not a vector parameter"));
				}
				FLinearColor Color = Vector->DefaultValue;
				if (!TryReadLinearColor(Params, TEXT("default_value"), Color, Error))
				{
					return MakeFailure(Error);
				}
				Vector->DefaultValue = Color;
			}
			break;
		case EMaterialParameterNodeType::Texture:
			{
				UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Target);
				if (!Texture)
				{
					return MakeFailure(TEXT("Target parameter is not a texture parameter"));
				}
				FString TexturePath;
				if (!Params->TryGetStringField(TEXT("texture_path"), TexturePath) && !Params->TryGetStringField(TEXT("default_texture_path"), TexturePath))
				{
					return MakeFailure(TEXT("Texture parameter requires 'texture_path' or 'default_texture_path'"));
				}
				TexturePath = TexturePath.TrimStartAndEnd();
				Texture->Texture = TexturePath.IsEmpty() ? nullptr : LoadAssetAs<UTexture>(TexturePath);
				if (!TexturePath.IsEmpty() && !Texture->Texture)
				{
					return MakeFailure(FString::Printf(TEXT("Texture asset not found: %s"), *TexturePath));
				}
			}
			break;
		case EMaterialParameterNodeType::StaticSwitch:
			{
				UMaterialExpressionStaticSwitchParameter* StaticSwitch = Cast<UMaterialExpressionStaticSwitchParameter>(Target);
				bool bDefaultValue = false;
				if (!StaticSwitch || !Params->TryGetBoolField(TEXT("default_value"), bDefaultValue))
				{
					return MakeFailure(TEXT("Static switch parameter requires boolean field 'default_value'"));
				}
				StaticSwitch->DefaultValue = bDefaultValue ? 1 : 0;
			}
			break;
		case EMaterialParameterNodeType::StaticComponentMask:
			{
				UMaterialExpressionStaticComponentMaskParameter* StaticMask = Cast<UMaterialExpressionStaticComponentMaskParameter>(Target);
				if (!StaticMask)
				{
					return MakeFailure(TEXT("Target parameter is not a static component mask parameter"));
				}
				bool bR = false;
				bool bG = false;
				bool bB = false;
				bool bA = false;
				if (!TryReadStaticComponentMaskDefaults(Params, bR, bG, bB, bA, Error))
				{
					return MakeFailure(Error);
				}
				StaticMask->DefaultR = bR ? 1 : 0;
				StaticMask->DefaultG = bG ? 1 : 0;
				StaticMask->DefaultB = bB ? 1 : 0;
				StaticMask->DefaultA = bA ? 1 : 0;
			}
			break;
		case EMaterialParameterNodeType::RuntimeVirtualTexture:
			{
				UMaterialExpressionRuntimeVirtualTextureSampleParameter* RuntimeVirtualTexture = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Target);
				if (!RuntimeVirtualTexture)
				{
					return MakeFailure(TEXT("Target parameter is not a runtime virtual texture parameter"));
				}
				FString RuntimeVirtualTexturePath;
				if (!Params->TryGetStringField(TEXT("runtime_virtual_texture_path"), RuntimeVirtualTexturePath) && !Params->TryGetStringField(TEXT("virtual_texture_path"), RuntimeVirtualTexturePath))
				{
					return MakeFailure(TEXT("Runtime virtual texture parameter requires 'runtime_virtual_texture_path' or 'virtual_texture_path'"));
				}
				RuntimeVirtualTexturePath = RuntimeVirtualTexturePath.TrimStartAndEnd();
				RuntimeVirtualTexture->VirtualTexture = RuntimeVirtualTexturePath.IsEmpty() ? nullptr : LoadAssetAs<URuntimeVirtualTexture>(RuntimeVirtualTexturePath);
				if (!RuntimeVirtualTexturePath.IsEmpty() && !RuntimeVirtualTexture->VirtualTexture)
				{
					return MakeFailure(FString::Printf(TEXT("Runtime virtual texture asset not found: %s"), *RuntimeVirtualTexturePath));
				}
			}
			break;
		case EMaterialParameterNodeType::Font:
			{
				UMaterialExpressionFontSampleParameter* Font = Cast<UMaterialExpressionFontSampleParameter>(Target);
				if (!Font)
				{
					return MakeFailure(TEXT("Target parameter is not a font parameter"));
				}

				FString FontPath;
				if (Params->TryGetStringField(TEXT("font_path"), FontPath) || Params->TryGetStringField(TEXT("default_font_path"), FontPath))
				{
					FontPath = FontPath.TrimStartAndEnd();
					Font->Font = FontPath.IsEmpty() ? nullptr : LoadAssetAs<UFont>(FontPath);
					if (!FontPath.IsEmpty() && !Font->Font)
					{
						return MakeFailure(FString::Printf(TEXT("Font asset not found: %s"), *FontPath));
					}
				}
				else
				{
					return MakeFailure(TEXT("Font parameter requires 'font_path' or 'default_font_path'"));
				}

				double FontPageDouble = 0.0;
				if (Params->TryGetNumberField(TEXT("font_page"), FontPageDouble))
				{
					Font->FontTexturePage = static_cast<int32>(FontPageDouble);
				}
			}
			break;
		default:
			return MakeFailure(TEXT("Target node is not a supported parameter expression"));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetParameterMetadata(const FMCPRequest& Request)
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

	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString ParameterName;
	Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName);

	FString GroupName;
	const bool bHasGroup = Request.Params->TryGetStringField(TEXT("group"), GroupName);

	bool bHasSortPriority = false;
	int32 SortPriority = 0;
	double SortPriorityDouble = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("sort_priority"), SortPriorityDouble))
	{
		bHasSortPriority = true;
		SortPriority = static_cast<int32>(SortPriorityDouble);
	}

	FString Description;
	const bool bHasDescription = Request.Params->TryGetStringField(TEXT("description"), Description);
	if (!bHasGroup && !bHasSortPriority && !bHasDescription)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one metadata field: group, sort_priority, or description"));
	}

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	auto Task = [AssetPath, NodeId, ParameterName, bHasGroup, GroupName, bHasSortPriority, SortPriority, bHasDescription, Description, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_parameter_metadata only supports UMaterial assets"));
		}

		UMaterialExpression* Target = ResolveParameterExpression(Context, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		Target->Modify();
		bool bChanged = false;
		if (bHasGroup)
		{
			bChanged = SetParameterExpressionGroup(Target, GroupName.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*GroupName.TrimStartAndEnd())) || bChanged;
		}
		if (bHasSortPriority)
		{
			bChanged = SetParameterExpressionSortPriority(Target, SortPriority) || bChanged;
		}
		if (bHasDescription)
		{
			Target->Desc = Description;
			bChanged = true;
		}

		if (bChanged)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleSetParameterChannelNames(const FMCPRequest& Request)
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

	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString ParameterName;
	Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName);

	bool bHasTypeFilter = false;
	EMaterialParameterNodeType TypeFilter = EMaterialParameterNodeType::Unknown;
	FString TypeFilterError;
	if (!TryResolveParameterTypeFilter(Request.Params, bHasTypeFilter, TypeFilter, TypeFilterError))
	{
		return InvalidParams(Request.Id, TypeFilterError);
	}

	const TSharedPtr<FJsonObject> Params = Request.Params;
	auto Task = [AssetPath, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Params]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/set_parameter_channel_names only supports UMaterial assets"));
		}

		UMaterialExpression* Target = ResolveParameterExpression(Context, NodeId, ParameterName, bHasTypeFilter, TypeFilter, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		const EMaterialParameterNodeType TargetType = GetParameterNodeType(Target);
		bool bAnyFieldsSet = false;
		if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Target))
		{
			FParameterChannelNames ChannelNames = Vector->ChannelNames;
			TryApplyChannelNameFields(Params, ChannelNames, bAnyFieldsSet);
			if (!bAnyFieldsSet)
			{
				return MakeFailure(TEXT("Provide channel names via channel_names{r,g,b,a} or channel_r/channel_g/channel_b/channel_a"));
			}

			Vector->Modify();
			Vector->ChannelNames = ChannelNames;
		}
		else if (UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Target))
		{
			FParameterChannelNames ChannelNames = Texture->ChannelNames;
			TryApplyChannelNameFields(Params, ChannelNames, bAnyFieldsSet);
			if (!bAnyFieldsSet)
			{
				return MakeFailure(TEXT("Provide channel names via channel_names{r,g,b,a} or channel_r/channel_g/channel_b/channel_a"));
			}

			Texture->Modify();
			Texture->ChannelNames = ChannelNames;
			Texture->ApplyChannelNames();
		}
		else
		{
			return MakeFailure(FString::Printf(TEXT("Parameter type '%s' does not support channel names"), *ParameterTypeToString(TargetType)));
		}

		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetParent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}

	FString ParentPath;
	if (!Request.Params->TryGetStringField(TEXT("parent_path"), ParentPath))
	{
		Request.Params->TryGetStringField(TEXT("parent_material_path"), ParentPath);
	}

	auto Task = [MaterialInstancePath, ParentPath]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		UMaterialInterface* PreviousParent = Instance->Parent;
		UMaterialInterface* NewParent = nullptr;

		const FString TrimmedParentPath = ParentPath.TrimStartAndEnd();
		if (!TrimmedParentPath.IsEmpty())
		{
			NewParent = LoadAssetAs<UMaterialInterface>(TrimmedParentPath);
			if (!NewParent)
			{
				return MakeFailure(FString::Printf(TEXT("Parent material/interface not found: %s"), *TrimmedParentPath));
			}
		}

		Instance->Modify();
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, NewParent);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetStringField(TEXT("previous_parent_path"), PreviousParent ? PreviousParent->GetPathName() : FString());
		Result->SetStringField(TEXT("parent_path"), Instance->Parent ? Instance->Parent->GetPathName() : FString());
		Result->SetBoolField(TEXT("changed"), PreviousParent != Instance->Parent);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceListOverrides(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}

	bool bIncludeStaticNonOverrides = false;
	Request.Params->TryGetBoolField(TEXT("include_static_non_overrides"), bIncludeStaticNonOverrides);

	EMaterialInstanceOverrideType TypeFilter = EMaterialInstanceOverrideType::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalMaterialInstanceOverrideType(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	auto Task = [MaterialInstancePath, bHasTypeFilter, TypeFilter, bIncludeStaticNonOverrides]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		TArray<TSharedPtr<FJsonValue>> ScalarOverrides;
		TArray<TSharedPtr<FJsonValue>> VectorOverrides;
		TArray<TSharedPtr<FJsonValue>> TextureOverrides;
		TArray<TSharedPtr<FJsonValue>> StaticSwitchOverrides;
		TArray<TSharedPtr<FJsonValue>> StaticComponentMaskOverrides;

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Scalar)
		{
			for (const FScalarParameterValue& Value : Instance->ScalarParameterValues)
			{
				ScalarOverrides.Add(MakeShared<FJsonValueObject>(BuildScalarOverrideJson(Value)));
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Vector)
		{
			for (const FVectorParameterValue& Value : Instance->VectorParameterValues)
			{
				VectorOverrides.Add(MakeShared<FJsonValueObject>(BuildVectorOverrideJson(Value)));
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Texture)
		{
			for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
			{
				TextureOverrides.Add(MakeShared<FJsonValueObject>(BuildTextureOverrideJson(Value)));
			}
		}

		FStaticParameterSet StaticParameters = Instance->GetStaticParameters();

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::StaticSwitch)
		{
			for (const FStaticSwitchParameter& Value : StaticParameters.StaticSwitchParameters)
			{
				if (bIncludeStaticNonOverrides || Value.bOverride)
				{
					StaticSwitchOverrides.Add(MakeShared<FJsonValueObject>(BuildStaticSwitchOverrideJson(Value)));
				}
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::StaticComponentMask)
		{
			for (const FStaticComponentMaskParameter& Value : StaticParameters.EditorOnly.StaticComponentMaskParameters)
			{
				if (bIncludeStaticNonOverrides || Value.bOverride)
				{
					StaticComponentMaskOverrides.Add(MakeShared<FJsonValueObject>(BuildStaticComponentMaskOverrideJson(Value)));
				}
			}
		}

		const int32 TotalOverrideCount =
			ScalarOverrides.Num() +
			VectorOverrides.Num() +
			TextureOverrides.Num() +
			StaticSwitchOverrides.Num() +
			StaticComponentMaskOverrides.Num();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetStringField(TEXT("parent_path"), Instance->Parent ? Instance->Parent->GetPathName() : FString());
		if (bHasTypeFilter)
		{
			Result->SetStringField(TEXT("parameter_type_filter"), MaterialInstanceOverrideTypeToString(TypeFilter));
		}
		Result->SetBoolField(TEXT("include_static_non_overrides"), bIncludeStaticNonOverrides);
		Result->SetArrayField(TEXT("scalar_overrides"), ScalarOverrides);
		Result->SetArrayField(TEXT("vector_overrides"), VectorOverrides);
		Result->SetArrayField(TEXT("texture_overrides"), TextureOverrides);
		Result->SetArrayField(TEXT("static_switch_overrides"), StaticSwitchOverrides);
		Result->SetArrayField(TEXT("static_component_mask_overrides"), StaticComponentMaskOverrides);
		Result->SetNumberField(TEXT("scalar_override_count"), ScalarOverrides.Num());
		Result->SetNumberField(TEXT("vector_override_count"), VectorOverrides.Num());
		Result->SetNumberField(TEXT("texture_override_count"), TextureOverrides.Num());
		Result->SetNumberField(TEXT("static_switch_override_count"), StaticSwitchOverrides.Num());
		Result->SetNumberField(TEXT("static_component_mask_override_count"), StaticComponentMaskOverrides.Num());
		Result->SetNumberField(TEXT("total_override_count"), TotalOverrideCount);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceClearOverride(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	ParameterName = ParameterName.TrimStartAndEnd();
	if (ParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	bool bHasAssociation = false;
	bool bHasIndex = false;
	FString AssociationError;
	if (!ParseOptionalMaterialParameterAssociationAndIndex(Request.Params, Association, ParameterIndex, bHasAssociation, bHasIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	EMaterialInstanceOverrideType TypeFilter = EMaterialInstanceOverrideType::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalMaterialInstanceOverrideType(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	auto Task = [MaterialInstancePath, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		bool bChanged = false;
		bool bStaticChanged = false;
		TArray<TSharedPtr<FJsonValue>> ClearedOverrides;

		auto AddCleared = [&ClearedOverrides](const TSharedPtr<FJsonObject>& Override)
		{
			ClearedOverrides.Add(MakeShared<FJsonValueObject>(Override));
		};

		Instance->Modify();

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Scalar)
		{
			for (int32 Index = Instance->ScalarParameterValues.Num() - 1; Index >= 0; --Index)
			{
				const FScalarParameterValue& Value = Instance->ScalarParameterValues[Index];
				if (MatchesMaterialParameterInfo(Value.ParameterInfo, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex))
				{
					AddCleared(BuildScalarOverrideJson(Value));
					Instance->ScalarParameterValues.RemoveAt(Index);
					bChanged = true;
				}
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Vector)
		{
			for (int32 Index = Instance->VectorParameterValues.Num() - 1; Index >= 0; --Index)
			{
				const FVectorParameterValue& Value = Instance->VectorParameterValues[Index];
				if (MatchesMaterialParameterInfo(Value.ParameterInfo, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex))
				{
					AddCleared(BuildVectorOverrideJson(Value));
					Instance->VectorParameterValues.RemoveAt(Index);
					bChanged = true;
				}
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::Texture)
		{
			for (int32 Index = Instance->TextureParameterValues.Num() - 1; Index >= 0; --Index)
			{
				const FTextureParameterValue& Value = Instance->TextureParameterValues[Index];
				if (MatchesMaterialParameterInfo(Value.ParameterInfo, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex))
				{
					AddCleared(BuildTextureOverrideJson(Value));
					Instance->TextureParameterValues.RemoveAt(Index);
					bChanged = true;
				}
			}
		}

		FStaticParameterSet StaticParameters = Instance->GetStaticParameters();

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::StaticSwitch)
		{
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> ParentStaticSwitchValues;
			if (Instance->Parent)
			{
				Instance->Parent->GetAllParametersOfType(EMaterialParameterType::StaticSwitch, ParentStaticSwitchValues);
			}

			for (FStaticSwitchParameter& Value : StaticParameters.StaticSwitchParameters)
			{
				if (!Value.bOverride)
				{
					continue;
				}
				if (!MatchesMaterialParameterInfo(Value.ParameterInfo, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex))
				{
					continue;
				}

				AddCleared(BuildStaticSwitchOverrideJson(Value));
				if (const FMaterialParameterMetadata* ParentMeta = ParentStaticSwitchValues.Find(Value.ParameterInfo))
				{
					if (ParentMeta->Value.Type == EMaterialParameterType::StaticSwitch)
					{
						Value.Value = ParentMeta->Value.AsStaticSwitch();
					}
				}
				Value.bOverride = false;
				bChanged = true;
				bStaticChanged = true;
			}
		}

		if (!bHasTypeFilter || TypeFilter == EMaterialInstanceOverrideType::StaticComponentMask)
		{
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> ParentStaticMaskValues;
			if (Instance->Parent)
			{
				Instance->Parent->GetAllParametersOfType(EMaterialParameterType::StaticComponentMask, ParentStaticMaskValues);
			}

			for (FStaticComponentMaskParameter& Value : StaticParameters.EditorOnly.StaticComponentMaskParameters)
			{
				if (!Value.bOverride)
				{
					continue;
				}
				if (!MatchesMaterialParameterInfo(Value.ParameterInfo, ParameterName, bHasAssociation, Association, bHasIndex, ParameterIndex))
				{
					continue;
				}

				AddCleared(BuildStaticComponentMaskOverrideJson(Value));
				if (const FMaterialParameterMetadata* ParentMeta = ParentStaticMaskValues.Find(Value.ParameterInfo))
				{
					if (ParentMeta->Value.Type == EMaterialParameterType::StaticComponentMask)
					{
						const FStaticComponentMaskValue ParentMask = ParentMeta->Value.AsStaticComponentMask();
						Value.R = ParentMask.R;
						Value.G = ParentMask.G;
						Value.B = ParentMask.B;
						Value.A = ParentMask.A;
					}
				}
				Value.bOverride = false;
				bChanged = true;
				bStaticChanged = true;
			}
		}

		if (bStaticChanged)
		{
			Instance->UpdateStaticPermutation(StaticParameters);
		}

		if (bChanged)
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetArrayField(TEXT("cleared_overrides"), ClearedOverrides);
		Result->SetNumberField(TEXT("cleared_count"), ClearedOverrides.Num());
		if (bHasTypeFilter)
		{
			Result->SetStringField(TEXT("parameter_type"), MaterialInstanceOverrideTypeToString(TypeFilter));
		}
		if (bHasAssociation)
		{
			Result->SetStringField(TEXT("association"), MaterialParameterAssociationToString(Association));
		}
		if (bHasIndex)
		{
			Result->SetNumberField(TEXT("index"), ParameterIndex);
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetScalar(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	double ScalarValue = 0.0;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	if (!Request.Params->TryGetNumberField(TEXT("value"), ScalarValue))
	{
		return InvalidParams(Request.Id, TEXT("Missing required numeric parameter 'value'"));
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	FString AssociationError;
	if (!ResolveMaterialParameterAssociationAndIndexForSet(Request.Params, Association, ParameterIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	auto Task = [MaterialInstancePath, TrimmedParameterName, ScalarValue, Association, ParameterIndex]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		const FMaterialParameterInfo ParameterInfo(FName(*TrimmedParameterName), Association, ParameterIndex);
		Instance->Modify();
		Instance->SetScalarParameterValueEditorOnly(ParameterInfo, static_cast<float>(ScalarValue));
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		const FScalarParameterValue* Override = Instance->ScalarParameterValues.FindByPredicate(
			[&ParameterInfo](const FScalarParameterValue& Value)
			{
				return Value.ParameterInfo == ParameterInfo;
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(ParameterInfo));
		Result->SetNumberField(TEXT("value"), static_cast<float>(ScalarValue));
		Result->SetBoolField(TEXT("override_found"), Override != nullptr);
		if (Override)
		{
			Result->SetObjectField(TEXT("override"), BuildScalarOverrideJson(*Override));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetVector(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}

	FLinearColor VectorValue = FLinearColor::Black;
	FString VectorParseError;
	if (!TryReadLinearColor(Request.Params, TEXT("value"), VectorValue, VectorParseError))
	{
		return InvalidParams(Request.Id, VectorParseError);
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	FString AssociationError;
	if (!ResolveMaterialParameterAssociationAndIndexForSet(Request.Params, Association, ParameterIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	auto Task = [MaterialInstancePath, TrimmedParameterName, VectorValue, Association, ParameterIndex]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		const FMaterialParameterInfo ParameterInfo(FName(*TrimmedParameterName), Association, ParameterIndex);
		Instance->Modify();
		Instance->SetVectorParameterValueEditorOnly(ParameterInfo, VectorValue);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		const FVectorParameterValue* Override = Instance->VectorParameterValues.FindByPredicate(
			[&ParameterInfo](const FVectorParameterValue& Value)
			{
				return Value.ParameterInfo == ParameterInfo;
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(ParameterInfo));
		Result->SetObjectField(TEXT("value"), BuildColorJson(VectorValue));
		Result->SetBoolField(TEXT("override_found"), Override != nullptr);
		if (Override)
		{
			Result->SetObjectField(TEXT("override"), BuildVectorOverrideJson(*Override));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetTexture(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}

	FString TexturePath;
	if (!Request.Params->TryGetStringField(TEXT("texture_path"), TexturePath))
	{
		Request.Params->TryGetStringField(TEXT("value"), TexturePath);
	}
	if (!Request.Params->HasField(TEXT("texture_path")) && !Request.Params->HasField(TEXT("value")))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'texture_path' (or 'value')"));
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	FString AssociationError;
	if (!ResolveMaterialParameterAssociationAndIndexForSet(Request.Params, Association, ParameterIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	auto Task = [MaterialInstancePath, TrimmedParameterName, TexturePath, Association, ParameterIndex]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		UTexture* Texture = nullptr;
		const FString TrimmedTexturePath = TexturePath.TrimStartAndEnd();
		if (!TrimmedTexturePath.IsEmpty())
		{
			Texture = LoadAssetAs<UTexture>(TrimmedTexturePath);
			if (!Texture)
			{
				return MakeFailure(FString::Printf(TEXT("Texture asset not found: %s"), *TrimmedTexturePath));
			}
		}

		const FMaterialParameterInfo ParameterInfo(FName(*TrimmedParameterName), Association, ParameterIndex);
		Instance->Modify();
		Instance->SetTextureParameterValueEditorOnly(ParameterInfo, Texture);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		const FTextureParameterValue* Override = Instance->TextureParameterValues.FindByPredicate(
			[&ParameterInfo](const FTextureParameterValue& Value)
			{
				return Value.ParameterInfo == ParameterInfo;
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(ParameterInfo));
		Result->SetStringField(TEXT("value"), Texture ? Texture->GetPathName() : FString());
		Result->SetBoolField(TEXT("override_found"), Override != nullptr);
		if (Override)
		{
			Result->SetObjectField(TEXT("override"), BuildTextureOverrideJson(*Override));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetStaticSwitch(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	bool bValue = false;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	if (!Request.Params->TryGetBoolField(TEXT("value"), bValue))
	{
		return InvalidParams(Request.Id, TEXT("Missing required boolean parameter 'value'"));
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	FString AssociationError;
	if (!ResolveMaterialParameterAssociationAndIndexForSet(Request.Params, Association, ParameterIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	auto Task = [MaterialInstancePath, TrimmedParameterName, bValue, Association, ParameterIndex]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		const FMaterialParameterInfo ParameterInfo(FName(*TrimmedParameterName), Association, ParameterIndex);
		FStaticParameterSet StaticParameters = Instance->GetStaticParameters();

		bool bFound = false;
		for (FStaticSwitchParameter& StaticSwitch : StaticParameters.StaticSwitchParameters)
		{
			if (StaticSwitch.ParameterInfo == ParameterInfo)
			{
				StaticSwitch.Value = bValue;
				StaticSwitch.bOverride = true;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			StaticParameters.StaticSwitchParameters.Emplace(ParameterInfo, bValue, true, FGuid());
		}

		Instance->Modify();
		Instance->UpdateStaticPermutation(StaticParameters);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		const FStaticSwitchParameter* Override = StaticParameters.StaticSwitchParameters.FindByPredicate(
			[&ParameterInfo](const FStaticSwitchParameter& Value)
			{
				return Value.ParameterInfo == ParameterInfo;
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(ParameterInfo));
		Result->SetBoolField(TEXT("value"), bValue);
		Result->SetBoolField(TEXT("override_found"), Override != nullptr);
		if (Override)
		{
			Result->SetObjectField(TEXT("override"), BuildStaticSwitchOverrideJson(*Override));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceSetStaticComponentMask(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString MaterialInstancePath;
	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), MaterialInstancePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}

	bool bR = false;
	bool bG = false;
	bool bB = false;
	bool bA = false;
	FString MaskParseError;
	if (!TryReadMaterialInstanceMaskValue(Request.Params, bR, bG, bB, bA, MaskParseError))
	{
		return InvalidParams(Request.Id, MaskParseError);
	}

	EMaterialParameterAssociation Association = EMaterialParameterAssociation::GlobalParameter;
	int32 ParameterIndex = INDEX_NONE;
	FString AssociationError;
	if (!ResolveMaterialParameterAssociationAndIndexForSet(Request.Params, Association, ParameterIndex, AssociationError))
	{
		return InvalidParams(Request.Id, AssociationError);
	}

	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	auto Task = [MaterialInstancePath, TrimmedParameterName, bR, bG, bB, bA, Association, ParameterIndex]() -> TSharedPtr<FJsonObject>
	{
		FString AssetPath;
		FString Error;
		UMaterialInstanceConstant* Instance = ResolveMaterialInstance(MaterialInstancePath, AssetPath, Error);
		if (!Instance)
		{
			return MakeFailure(Error);
		}

		const FMaterialParameterInfo ParameterInfo(FName(*TrimmedParameterName), Association, ParameterIndex);
		FStaticParameterSet StaticParameters = Instance->GetStaticParameters();

		bool bFound = false;
		for (FStaticComponentMaskParameter& StaticMask : StaticParameters.EditorOnly.StaticComponentMaskParameters)
		{
			if (StaticMask.ParameterInfo == ParameterInfo)
			{
				StaticMask.R = bR;
				StaticMask.G = bG;
				StaticMask.B = bB;
				StaticMask.A = bA;
				StaticMask.bOverride = true;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			StaticParameters.EditorOnly.StaticComponentMaskParameters.Emplace(ParameterInfo, bR, bG, bB, bA, true, FGuid());
		}

		Instance->Modify();
		Instance->UpdateStaticPermutation(StaticParameters);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

		const FStaticComponentMaskParameter* Override = StaticParameters.EditorOnly.StaticComponentMaskParameters.FindByPredicate(
			[&ParameterInfo](const FStaticComponentMaskParameter& Value)
			{
				return Value.ParameterInfo == ParameterInfo;
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), AssetPath);
		Result->SetObjectField(TEXT("parameter_info"), BuildMaterialParameterInfoJson(ParameterInfo));
		Result->SetObjectField(TEXT("value"), BuildMaskJson(bR, bG, bB, bA));
		Result->SetBoolField(TEXT("override_found"), Override != nullptr);
		if (Override)
		{
			Result->SetObjectField(TEXT("override"), BuildStaticComponentMaskOverrideJson(*Override));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialInstanceCopyOverridesFromInstance(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString DestinationPath;
	FString SourcePath;
	if (!Request.Params->TryGetStringField(TEXT("material_instance_path"), DestinationPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_instance_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("source_material_instance_path"), SourcePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'source_material_instance_path'"));
	}

	auto Task = [DestinationPath, SourcePath]() -> TSharedPtr<FJsonObject>
	{
		FString DestinationAssetPath;
		FString Error;
		UMaterialInstanceConstant* Destination = ResolveMaterialInstance(DestinationPath, DestinationAssetPath, Error);
		if (!Destination)
		{
			return MakeFailure(Error);
		}

		FString SourceAssetPath;
		UMaterialInstanceConstant* Source = ResolveMaterialInstance(SourcePath, SourceAssetPath, Error);
		if (!Source)
		{
			return MakeFailure(Error);
		}

		if (Destination == Source)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("material_instance_path"), DestinationAssetPath);
			Result->SetStringField(TEXT("source_material_instance_path"), SourceAssetPath);
			Result->SetBoolField(TEXT("changed"), false);
			return Result;
		}

		Destination->Modify();
		Destination->CopyMaterialUniformParametersEditorOnly(Source, true);
		UMaterialEditingLibrary::UpdateMaterialInstance(Destination);

		FStaticParameterSet StaticParameters = Destination->GetStaticParameters();
		int32 StaticSwitchOverrideCount = 0;
		for (const FStaticSwitchParameter& StaticSwitch : StaticParameters.StaticSwitchParameters)
		{
			if (StaticSwitch.bOverride)
			{
				++StaticSwitchOverrideCount;
			}
		}

		int32 StaticMaskOverrideCount = 0;
		for (const FStaticComponentMaskParameter& StaticMask : StaticParameters.EditorOnly.StaticComponentMaskParameters)
		{
			if (StaticMask.bOverride)
			{
				++StaticMaskOverrideCount;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_instance_path"), DestinationAssetPath);
		Result->SetStringField(TEXT("source_material_instance_path"), SourceAssetPath);
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetNumberField(TEXT("scalar_override_count"), Destination->ScalarParameterValues.Num());
		Result->SetNumberField(TEXT("vector_override_count"), Destination->VectorParameterValues.Num());
		Result->SetNumberField(TEXT("texture_override_count"), Destination->TextureParameterValues.Num());
		Result->SetNumberField(TEXT("static_switch_override_count"), StaticSwitchOverrideCount);
		Result->SetNumberField(TEXT("static_component_mask_override_count"), StaticMaskOverrideCount);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionGetInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	auto Task = [FunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		TArray<UMaterialExpressionFunctionInput*> Inputs;
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		GatherMaterialFunctionIONodes(Function, Inputs, Outputs);

		int32 FunctionCallNodeCount = 0;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (Expression && Expression->IsA<UMaterialExpressionMaterialFunctionCall>())
			{
				++FunctionCallNodeCount;
			}
		}

		TArray<UMaterialFunctionInterface*> DependentFunctions;
		Function->GetDependentFunctions(DependentFunctions);

		TArray<TSharedPtr<FJsonValue>> LibraryCategories;
		for (const FText& CategoryText : Function->LibraryCategoriesText)
		{
			LibraryCategories.Add(MakeShared<FJsonValueString>(CategoryText.ToString()));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetStringField(TEXT("asset_class"), Function->GetClass()->GetPathName());
		Result->SetStringField(TEXT("description"), Function->Description);
		Result->SetStringField(TEXT("user_exposed_caption"), Function->UserExposedCaption);
		Result->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary != 0);
		Result->SetArrayField(TEXT("library_categories"), LibraryCategories);
		Result->SetNumberField(TEXT("input_count"), Inputs.Num());
		Result->SetNumberField(TEXT("output_count"), Outputs.Num());
		Result->SetNumberField(TEXT("function_call_node_count"), FunctionCallNodeCount);
		Result->SetNumberField(TEXT("dependent_function_count"), DependentFunctions.Num());
		Result->SetStringField(TEXT("state_id"), Function->StateId.IsValid() ? Function->StateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Result->SetStringField(TEXT("preview_material_domain"), DomainToString(Function->PreviewMaterialDomain));
		Result->SetStringField(TEXT("preview_blend_mode"), BlendModeToString(Function->PreviewBlendMode));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionListInputs(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	auto Task = [FunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		TArray<UMaterialExpressionFunctionInput*> Inputs;
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		GatherMaterialFunctionIONodes(Function, Inputs, Outputs);

		TArray<TSharedPtr<FJsonValue>> InputArray;
		for (UMaterialExpressionFunctionInput* InputExpression : Inputs)
		{
			InputArray.Add(MakeShared<FJsonValueObject>(BuildMaterialFunctionInputJson(InputExpression)));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetArrayField(TEXT("inputs"), InputArray);
		Result->SetNumberField(TEXT("input_count"), InputArray.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionListOutputs(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	auto Task = [FunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		TArray<UMaterialExpressionFunctionInput*> Inputs;
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		GatherMaterialFunctionIONodes(Function, Inputs, Outputs);

		TArray<TSharedPtr<FJsonValue>> OutputArray;
		for (UMaterialExpressionFunctionOutput* OutputExpression : Outputs)
		{
			OutputArray.Add(MakeShared<FJsonValueObject>(BuildMaterialFunctionOutputJson(OutputExpression)));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetArrayField(TEXT("outputs"), OutputArray);
		Result->SetNumberField(TEXT("output_count"), OutputArray.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionCreateInput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	FString InputName;
	Request.Params->TryGetStringField(TEXT("input_name"), InputName);
	const FString TrimmedInputName = InputName.TrimStartAndEnd();

	FString InputTypeText = TEXT("scalar");
	if (!Request.Params->TryGetStringField(TEXT("input_type"), InputTypeText))
	{
		Request.Params->TryGetStringField(TEXT("value_type"), InputTypeText);
	}

	EFunctionInputType InputType = FunctionInput_Scalar;
	FString TypeError;
	if (!ParseFunctionInputTypeToken(InputTypeText, InputType, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	FString Description;
	const bool bHasDescription = Request.Params->TryGetStringField(TEXT("description"), Description);

	double NumericValue = 0.0;
	bool bHasSortPriority = false;
	int32 SortPriority = 0;
	if (Request.Params->TryGetNumberField(TEXT("sort_priority"), NumericValue))
	{
		bHasSortPriority = true;
		SortPriority = static_cast<int32>(NumericValue);
	}

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NumericValue))
	{
		NodePosX = static_cast<int32>(NumericValue);
	}
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NumericValue))
	{
		NodePosY = static_cast<int32>(NumericValue);
	}

	bool bHasUsePreviewValueAsDefault = false;
	bool bUsePreviewValueAsDefault = false;
	if (Request.Params->HasField(TEXT("use_preview_value_as_default")))
	{
		bHasUsePreviewValueAsDefault = true;
		Request.Params->TryGetBoolField(TEXT("use_preview_value_as_default"), bUsePreviewValueAsDefault);
	}

	const bool bHasPreviewValue = Request.Params->HasField(TEXT("preview_value"));
	FLinearColor PreviewValue = FLinearColor::Black;
	if (bHasPreviewValue)
	{
		FString PreviewError;
		if (!TryReadLinearColor(Request.Params, TEXT("preview_value"), PreviewValue, PreviewError))
		{
			return InvalidParams(Request.Id, PreviewError);
		}
	}

	auto Task = [FunctionPath, TrimmedInputName, InputType, bHasDescription, Description, bHasSortPriority, SortPriority, NodePosX, NodePosY, bHasUsePreviewValueAsDefault, bUsePreviewValueAsDefault, bHasPreviewValue, PreviewValue]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			nullptr,
			Function,
			UMaterialExpressionFunctionInput::StaticClass(),
			nullptr,
			NodePosX,
			NodePosY,
			true);
		UMaterialExpressionFunctionInput* NewInput = Cast<UMaterialExpressionFunctionInput>(NewExpression);
		if (!NewInput)
		{
			return MakeFailure(TEXT("Failed to create material function input node"));
		}

		NewInput->Modify();
		NewInput->InputType = InputType;
		if (!TrimmedInputName.IsEmpty())
		{
			NewInput->InputName = FName(*TrimmedInputName);
			NewInput->ValidateName();
		}
		if (bHasDescription)
		{
			NewInput->Description = Description;
		}
		if (bHasSortPriority)
		{
			NewInput->SortPriority = SortPriority;
		}
		if (bHasUsePreviewValueAsDefault)
		{
			NewInput->bUsePreviewValueAsDefault = bUsePreviewValueAsDefault ? 1 : 0;
		}
		if (bHasPreviewValue)
		{
			NewInput->PreviewValue = FVector4f(PreviewValue.R, PreviewValue.G, PreviewValue.B, PreviewValue.A);
		}
		NewInput->ConditionallyGenerateId(true);
		NewInput->ValidateName();

		Function->UpdateInputOutputTypes();
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetObjectField(TEXT("input"), BuildMaterialFunctionInputJson(NewInput));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionCreateOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	FString OutputName;
	Request.Params->TryGetStringField(TEXT("output_name"), OutputName);
	const FString TrimmedOutputName = OutputName.TrimStartAndEnd();

	FString Description;
	const bool bHasDescription = Request.Params->TryGetStringField(TEXT("description"), Description);

	double NumericValue = 0.0;
	bool bHasSortPriority = false;
	int32 SortPriority = 0;
	if (Request.Params->TryGetNumberField(TEXT("sort_priority"), NumericValue))
	{
		bHasSortPriority = true;
		SortPriority = static_cast<int32>(NumericValue);
	}

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NumericValue))
	{
		NodePosX = static_cast<int32>(NumericValue);
	}
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NumericValue))
	{
		NodePosY = static_cast<int32>(NumericValue);
	}

	FString FromNodeId;
	Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId);
	FString FromOutputPin;
	Request.Params->TryGetStringField(TEXT("from_output_pin"), FromOutputPin);
	bool bHasFromOutputIndex = false;
	int32 FromOutputIndex = 0;
	if (Request.Params->TryGetNumberField(TEXT("from_output_index"), NumericValue))
	{
		bHasFromOutputIndex = true;
		FromOutputIndex = static_cast<int32>(NumericValue);
	}

	auto Task = [FunctionPath, TrimmedOutputName, bHasDescription, Description, bHasSortPriority, SortPriority, NodePosX, NodePosY, FromNodeId, FromOutputPin, bHasFromOutputIndex, FromOutputIndex]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			nullptr,
			Function,
			UMaterialExpressionFunctionOutput::StaticClass(),
			nullptr,
			NodePosX,
			NodePosY,
			true);
		UMaterialExpressionFunctionOutput* NewOutput = Cast<UMaterialExpressionFunctionOutput>(NewExpression);
		if (!NewOutput)
		{
			return MakeFailure(TEXT("Failed to create material function output node"));
		}

		NewOutput->Modify();
		if (!TrimmedOutputName.IsEmpty())
		{
			NewOutput->OutputName = FName(*TrimmedOutputName);
			NewOutput->ValidateName();
		}
		if (bHasDescription)
		{
			NewOutput->Description = Description;
		}
		if (bHasSortPriority)
		{
			NewOutput->SortPriority = SortPriority;
		}
		NewOutput->ConditionallyGenerateId(true);
		NewOutput->ValidateName();

		const FString TrimmedFromNodeId = FromNodeId.TrimStartAndEnd();
		if (!TrimmedFromNodeId.IsEmpty())
		{
			FMaterialGraphContext Context;
			Context.MaterialFunction = Function;
			Context.AssetPath = FunctionAssetPath;

			UMaterialExpression* FromNode = FindNodeById(Context, TrimmedFromNodeId);
			if (!FromNode)
			{
				UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, NewOutput);
				return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *TrimmedFromNodeId));
			}
			if (FromNode->IsA<UMaterialExpressionComment>())
			{
				UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, NewOutput);
				return MakeFailure(TEXT("Cannot connect from a comment node"));
			}

			int32 ResolvedFromOutputIndex = INDEX_NONE;
			if (!TryResolveOutputPinIndex(FromNode, FromOutputPin, bHasFromOutputIndex, FromOutputIndex, ResolvedFromOutputIndex, Error))
			{
				UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, NewOutput);
				return MakeFailure(Error);
			}

			FromNode->Modify();
			FromNode->ConnectExpression(&NewOutput->A, ResolvedFromOutputIndex);
		}

		Function->UpdateInputOutputTypes();
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetObjectField(TEXT("output"), BuildMaterialFunctionOutputJson(NewOutput));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionRemoveInput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	FString NodeId;
	FString InputName;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	Request.Params->TryGetStringField(TEXT("input_name"), InputName);
	if (NodeId.TrimStartAndEnd().IsEmpty() && InputName.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Provide either 'node_id' or 'input_name'"));
	}

	auto Task = [FunctionPath, NodeId, InputName]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		UMaterialExpressionFunctionInput* Target = ResolveMaterialFunctionInputNode(Function, NodeId, InputName, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		const FString RemovedNodeId = GetNodeId(Target);
		const FString RemovedInputName = Target->InputName.ToString();
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Target);
		Function->UpdateInputOutputTypes();
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetStringField(TEXT("removed_node_id"), RemovedNodeId);
		Result->SetStringField(TEXT("removed_input_name"), RemovedInputName);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionRemoveOutput(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	FString NodeId;
	FString OutputName;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	Request.Params->TryGetStringField(TEXT("output_name"), OutputName);
	if (NodeId.TrimStartAndEnd().IsEmpty() && OutputName.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Provide either 'node_id' or 'output_name'"));
	}

	auto Task = [FunctionPath, NodeId, OutputName]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		UMaterialExpressionFunctionOutput* Target = ResolveMaterialFunctionOutputNode(Function, NodeId, OutputName, Error);
		if (!Target)
		{
			return MakeFailure(Error);
		}

		const FString RemovedNodeId = GetNodeId(Target);
		const FString RemovedOutputName = Target->OutputName.ToString();
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Target);
		Function->UpdateInputOutputTypes();
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetStringField(TEXT("removed_node_id"), RemovedNodeId);
		Result->SetStringField(TEXT("removed_output_name"), RemovedOutputName);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionAddCallNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString GraphAssetPath;
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), GraphAssetPath))
	{
		Request.Params->TryGetStringField(TEXT("graph_asset_path"), GraphAssetPath);
	}
	if (GraphAssetPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("called_material_function_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	int32 NodePosX = 0;
	int32 NodePosY = 0;
	double NumericValue = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("node_pos_x"), NumericValue))
	{
		NodePosX = static_cast<int32>(NumericValue);
	}
	if (Request.Params->TryGetNumberField(TEXT("node_pos_y"), NumericValue))
	{
		NodePosY = static_cast<int32>(NumericValue);
	}

	auto Task = [GraphAssetPath, FunctionPath, NodePosX, NodePosY]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(GraphAssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		FString CalledFunctionAssetPath;
		UMaterialFunction* CalledFunction = ResolveMaterialFunctionAsset(FunctionPath, CalledFunctionAssetPath, Error);
		if (!CalledFunction)
		{
			return MakeFailure(Error);
		}

		UMaterialExpression* NewExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Context.Material,
			Context.MaterialFunction,
			UMaterialExpressionMaterialFunctionCall::StaticClass(),
			CalledFunction,
			NodePosX,
			NodePosY,
			true);
		UMaterialExpressionMaterialFunctionCall* CallNode = Cast<UMaterialExpressionMaterialFunctionCall>(NewExpression);
		if (!CallNode)
		{
			return MakeFailure(TEXT("Failed to create material function call node"));
		}

		if (!CallNode->SetMaterialFunction(CalledFunction))
		{
			if (Context.Material)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, CallNode);
			}
			else if (Context.MaterialFunction)
			{
				UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Context.MaterialFunction, CallNode);
			}
			return MakeFailure(FString::Printf(TEXT("Failed to assign material function to call node: %s"), *CalledFunctionAssetPath));
		}

		Context.MarkDirty();
		if (Context.Material)
		{
			UMaterialEditingLibrary::RecompileMaterial(Context.Material);
		}
		else if (Context.MaterialFunction)
		{
			Context.MaterialFunction->UpdateInputOutputTypes();
			UMaterialEditingLibrary::UpdateMaterialFunction(Context.MaterialFunction, nullptr);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("asset_type"), Context.Material ? TEXT("material") : TEXT("material_function"));
		Result->SetStringField(TEXT("called_material_function_path"), CalledFunctionAssetPath);
		Result->SetNumberField(TEXT("function_input_count"), CallNode->FunctionInputs.Num());
		Result->SetNumberField(TEXT("function_output_count"), CallNode->FunctionOutputs.Num());
		Result->SetObjectField(TEXT("node"), BuildNodeJson(CallNode));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionSetIOTypes(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	struct FIOTypeUpdate
	{
		bool bTargetOutput = false;
		FString NodeId;
		FString InputName;
		FString OutputName;
		EFunctionInputType InputType = FunctionInput_Scalar;
	};

	TArray<FIOTypeUpdate> Updates;
	const TArray<TSharedPtr<FJsonValue>>* UpdatesArray = nullptr;
	if (Request.Params->TryGetArrayField(TEXT("updates"), UpdatesArray) && UpdatesArray)
	{
		for (const TSharedPtr<FJsonValue>& UpdateValue : *UpdatesArray)
		{
			const TSharedPtr<FJsonObject>* UpdateObjectPtr = nullptr;
			if (!UpdateValue.IsValid() || !UpdateValue->TryGetObject(UpdateObjectPtr) || !UpdateObjectPtr || !UpdateObjectPtr->IsValid())
			{
				return InvalidParams(Request.Id, TEXT("Each item in 'updates' must be an object"));
			}
			const TSharedPtr<FJsonObject>& UpdateObject = *UpdateObjectPtr;

			FIOTypeUpdate Update;
			FString Kind;
			if (!UpdateObject->TryGetStringField(TEXT("io_kind"), Kind))
			{
				UpdateObject->TryGetStringField(TEXT("target_kind"), Kind);
			}
			const FString KindToken = NormalizeParameterToken(Kind.IsEmpty() ? TEXT("input") : Kind);
			if (KindToken == TEXT("output"))
			{
				Update.bTargetOutput = true;
			}
			else if (KindToken == TEXT("input"))
			{
				Update.bTargetOutput = false;
			}
			else
			{
				return InvalidParams(Request.Id, FString::Printf(TEXT("Invalid io_kind '%s' in updates"), *Kind));
			}

			UpdateObject->TryGetStringField(TEXT("node_id"), Update.NodeId);
			UpdateObject->TryGetStringField(TEXT("input_name"), Update.InputName);
			UpdateObject->TryGetStringField(TEXT("output_name"), Update.OutputName);

			FString TypeText;
			if (!UpdateObject->TryGetStringField(TEXT("input_type"), TypeText))
			{
				if (!UpdateObject->TryGetStringField(TEXT("value_type"), TypeText))
				{
					UpdateObject->TryGetStringField(TEXT("type"), TypeText);
				}
			}
			FString TypeError;
			if (!ParseFunctionInputTypeToken(TypeText, Update.InputType, TypeError))
			{
				return InvalidParams(Request.Id, FString::Printf(TEXT("Invalid update type: %s"), *TypeError));
			}

			if (Update.bTargetOutput)
			{
				if (Update.NodeId.TrimStartAndEnd().IsEmpty() && Update.OutputName.TrimStartAndEnd().IsEmpty())
				{
					return InvalidParams(Request.Id, TEXT("Output updates require 'node_id' or 'output_name'"));
				}
			}
			else if (Update.NodeId.TrimStartAndEnd().IsEmpty() && Update.InputName.TrimStartAndEnd().IsEmpty())
			{
				return InvalidParams(Request.Id, TEXT("Input updates require 'node_id' or 'input_name'"));
			}

			Updates.Add(Update);
		}
	}
	else
	{
		FIOTypeUpdate Update;
		FString Kind;
		Request.Params->TryGetStringField(TEXT("io_kind"), Kind);
		const FString KindToken = NormalizeParameterToken(Kind.IsEmpty() ? TEXT("input") : Kind);
		Update.bTargetOutput = KindToken == TEXT("output");
		if (!(Update.bTargetOutput || KindToken == TEXT("input")))
		{
			return InvalidParams(Request.Id, FString::Printf(TEXT("Invalid io_kind '%s'"), *Kind));
		}

		Request.Params->TryGetStringField(TEXT("node_id"), Update.NodeId);
		Request.Params->TryGetStringField(TEXT("input_name"), Update.InputName);
		Request.Params->TryGetStringField(TEXT("output_name"), Update.OutputName);

		FString TypeText;
		if (!Request.Params->TryGetStringField(TEXT("input_type"), TypeText))
		{
			if (!Request.Params->TryGetStringField(TEXT("value_type"), TypeText))
			{
				Request.Params->TryGetStringField(TEXT("type"), TypeText);
			}
		}
		FString TypeError;
		if (!ParseFunctionInputTypeToken(TypeText, Update.InputType, TypeError))
		{
			return InvalidParams(Request.Id, TypeError);
		}

		if (Update.bTargetOutput)
		{
			if (Update.NodeId.TrimStartAndEnd().IsEmpty() && Update.OutputName.TrimStartAndEnd().IsEmpty())
			{
				return InvalidParams(Request.Id, TEXT("Provide 'node_id' or 'output_name' when io_kind='output'"));
			}
		}
		else if (Update.NodeId.TrimStartAndEnd().IsEmpty() && Update.InputName.TrimStartAndEnd().IsEmpty())
		{
			return InvalidParams(Request.Id, TEXT("Provide 'node_id' or 'input_name' when io_kind='input'"));
		}

		Updates.Add(Update);
	}

	if (Updates.Num() == 0)
	{
		return InvalidParams(Request.Id, TEXT("No updates provided"));
	}

	auto Task = [FunctionPath, Updates]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		int32 ChangedCount = 0;
		TArray<TSharedPtr<FJsonValue>> AppliedUpdates;
		for (const FIOTypeUpdate& Update : Updates)
		{
			if (!Update.bTargetOutput)
			{
				UMaterialExpressionFunctionInput* InputExpression = ResolveMaterialFunctionInputNode(Function, Update.NodeId, Update.InputName, Error);
				if (!InputExpression)
				{
					return MakeFailure(Error);
				}

				const bool bChanged = InputExpression->InputType != Update.InputType;
				if (bChanged)
				{
					InputExpression->Modify();
					InputExpression->InputType = Update.InputType;
					++ChangedCount;
				}

				TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
				Applied->SetStringField(TEXT("io_kind"), TEXT("input"));
				Applied->SetStringField(TEXT("node_id"), GetNodeId(InputExpression));
				Applied->SetStringField(TEXT("input_name"), InputExpression->InputName.ToString());
				Applied->SetStringField(TEXT("input_type"), FunctionInputTypeToString(InputExpression->InputType));
				Applied->SetBoolField(TEXT("changed"), bChanged);
				AppliedUpdates.Add(MakeShared<FJsonValueObject>(Applied));
				continue;
			}

			UMaterialExpressionFunctionOutput* OutputExpression = ResolveMaterialFunctionOutputNode(Function, Update.NodeId, Update.OutputName, Error);
			if (!OutputExpression)
			{
				return MakeFailure(Error);
			}

			UMaterialExpressionFunctionInput* DriverInput = Cast<UMaterialExpressionFunctionInput>(OutputExpression->A.Expression);
			if (!DriverInput)
			{
				return MakeFailure(FString::Printf(
					TEXT("Output '%s' is not driven directly by a function input node. Connect it to a function input and retry."),
					*OutputExpression->OutputName.ToString()));
			}

			const bool bChanged = DriverInput->InputType != Update.InputType;
			if (bChanged)
			{
				DriverInput->Modify();
				DriverInput->InputType = Update.InputType;
				++ChangedCount;
			}

			TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
			Applied->SetStringField(TEXT("io_kind"), TEXT("output"));
			Applied->SetStringField(TEXT("node_id"), GetNodeId(OutputExpression));
			Applied->SetStringField(TEXT("output_name"), OutputExpression->OutputName.ToString());
			Applied->SetStringField(TEXT("driver_input_node_id"), GetNodeId(DriverInput));
			Applied->SetStringField(TEXT("driver_input_name"), DriverInput->InputName.ToString());
			Applied->SetStringField(TEXT("input_type"), FunctionInputTypeToString(DriverInput->InputType));
			Applied->SetBoolField(TEXT("changed"), bChanged);
			AppliedUpdates.Add(MakeShared<FJsonValueObject>(Applied));
		}

		if (ChangedCount > 0)
		{
			Function->UpdateInputOutputTypes();
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetNumberField(TEXT("requested_updates"), Updates.Num());
		Result->SetNumberField(TEXT("changed_updates"), ChangedCount);
		Result->SetArrayField(TEXT("updates"), AppliedUpdates);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialFunctionCompile(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("material_function_path"), FunctionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), FunctionPath);
	}
	if (FunctionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'material_function_path'"));
	}

	FString PreviewMaterialPath;
	Request.Params->TryGetStringField(TEXT("preview_material_path"), PreviewMaterialPath);
	const FString TrimmedPreviewMaterialPath = PreviewMaterialPath.TrimStartAndEnd();

	auto Task = [FunctionPath, TrimmedPreviewMaterialPath]() -> TSharedPtr<FJsonObject>
	{
		FString FunctionAssetPath;
		FString Error;
		UMaterialFunction* Function = ResolveMaterialFunctionAsset(FunctionPath, FunctionAssetPath, Error);
		if (!Function)
		{
			return MakeFailure(Error);
		}

		UMaterial* PreviewMaterial = nullptr;
		if (!TrimmedPreviewMaterialPath.IsEmpty())
		{
			PreviewMaterial = LoadAssetAs<UMaterial>(TrimmedPreviewMaterialPath);
			if (!PreviewMaterial)
			{
				return MakeFailure(FString::Printf(TEXT("Preview material not found: %s"), *TrimmedPreviewMaterialPath));
			}
		}

		const FGuid PreviousStateId = Function->StateId;
		Function->UpdateInputOutputTypes();
		UMaterialEditingLibrary::UpdateMaterialFunction(Function, PreviewMaterial);
		const FGuid CurrentStateId = Function->StateId;

		TArray<UMaterialFunctionInterface*> DependentFunctions;
		Function->GetDependentFunctions(DependentFunctions);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material_function_path"), FunctionAssetPath);
		Result->SetBoolField(TEXT("state_id_changed"), PreviousStateId != CurrentStateId);
		Result->SetStringField(TEXT("previous_state_id"), PreviousStateId.IsValid() ? PreviousStateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Result->SetStringField(TEXT("state_id"), CurrentStateId.IsValid() ? CurrentStateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Result->SetNumberField(TEXT("dependent_function_count"), DependentFunctions.Num());
		Result->SetStringField(TEXT("preview_material_path"), PreviewMaterial ? NormalizeAssetPath(PreviewMaterial->GetPathName()) : FString());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionGetInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	auto Task = [CollectionPath]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		TArray<const UMaterialParameterCollection*> Hierarchy;
		GatherCollectionHierarchy(Collection, Hierarchy);

		int32 AccessibleScalarCount = 0;
		int32 AccessibleVectorCount = 0;
		for (const UMaterialParameterCollection* Source : Hierarchy)
		{
			if (!Source)
			{
				continue;
			}
			AccessibleScalarCount += Source->ScalarParameters.Num();
			AccessibleVectorCount += Source->VectorParameters.Num();
		}

		TArray<FName> ScalarNames = Collection->GetScalarParameterNames();
		TArray<FName> VectorNames = Collection->GetVectorParameterNames();
		TArray<TSharedPtr<FJsonValue>> ScalarNameArray;
		TArray<TSharedPtr<FJsonValue>> VectorNameArray;
		for (const FName Name : ScalarNames)
		{
			ScalarNameArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		for (const FName Name : VectorNames)
		{
			VectorNameArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}

		const int32 DirectScalarCount = Collection->ScalarParameters.Num();
		const int32 DirectVectorCount = Collection->VectorParameters.Num();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetStringField(TEXT("asset_class"), Collection->GetClass()->GetPathName());
		Result->SetBoolField(TEXT("has_base_collection"), Collection->GetBaseParameterCollection() != nullptr);
		Result->SetStringField(TEXT("base_collection_path"), Collection->GetBaseParameterCollection() ? NormalizeAssetPath(Collection->GetBaseParameterCollection()->GetPathName()) : FString());
		Result->SetStringField(TEXT("state_id"), Collection->StateId.IsValid() ? Collection->StateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Result->SetNumberField(TEXT("scalar_parameter_count"), DirectScalarCount);
		Result->SetNumberField(TEXT("vector_parameter_count"), DirectVectorCount);
		Result->SetNumberField(TEXT("parameter_count"), DirectScalarCount + DirectVectorCount);
		Result->SetNumberField(TEXT("accessible_scalar_parameter_count"), AccessibleScalarCount);
		Result->SetNumberField(TEXT("accessible_vector_parameter_count"), AccessibleVectorCount);
		Result->SetNumberField(TEXT("accessible_parameter_count"), AccessibleScalarCount + AccessibleVectorCount);
		Result->SetNumberField(TEXT("inherited_scalar_parameter_count"), AccessibleScalarCount - DirectScalarCount);
		Result->SetNumberField(TEXT("inherited_vector_parameter_count"), AccessibleVectorCount - DirectVectorCount);
		Result->SetNumberField(TEXT("total_vector_storage"), Collection->GetTotalVectorStorage());
		Result->SetArrayField(TEXT("scalar_parameter_names"), ScalarNameArray);
		Result->SetArrayField(TEXT("vector_parameter_names"), VectorNameArray);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionListParameters(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	bool bIncludeInherited = true;
	Request.Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	ECollectionParameterKind TypeFilter = ECollectionParameterKind::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalCollectionParameterKind(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	auto Task = [CollectionPath, bIncludeInherited, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		TArray<const UMaterialParameterCollection*> Hierarchy;
		if (bIncludeInherited)
		{
			GatherCollectionHierarchy(Collection, Hierarchy);
		}
		else
		{
			Hierarchy.Add(Collection);
		}

		TArray<TSharedPtr<FJsonValue>> ScalarParameters;
		TArray<TSharedPtr<FJsonValue>> VectorParameters;
		TArray<TSharedPtr<FJsonValue>> AllParameters;
		for (const UMaterialParameterCollection* SourceCollection : Hierarchy)
		{
			if (!SourceCollection)
			{
				continue;
			}

			const FString SourcePath = NormalizeAssetPath(SourceCollection->GetPathName());
			const bool bInherited = SourceCollection != Collection;

			if (!bHasTypeFilter || TypeFilter == ECollectionParameterKind::Scalar)
			{
				for (const FCollectionScalarParameter& ScalarParameter : SourceCollection->ScalarParameters)
				{
					TSharedPtr<FJsonObject> ParameterObject = BuildCollectionScalarParameterJson(ScalarParameter, SourcePath, bInherited);
					ScalarParameters.Add(MakeShared<FJsonValueObject>(ParameterObject));
					AllParameters.Add(MakeShared<FJsonValueObject>(ParameterObject));
				}
			}

			if (!bHasTypeFilter || TypeFilter == ECollectionParameterKind::Vector)
			{
				for (const FCollectionVectorParameter& VectorParameter : SourceCollection->VectorParameters)
				{
					TSharedPtr<FJsonObject> ParameterObject = BuildCollectionVectorParameterJson(VectorParameter, SourcePath, bInherited);
					VectorParameters.Add(MakeShared<FJsonValueObject>(ParameterObject));
					AllParameters.Add(MakeShared<FJsonValueObject>(ParameterObject));
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetBoolField(TEXT("include_inherited"), bIncludeInherited);
		Result->SetStringField(TEXT("parameter_type_filter"), bHasTypeFilter ? CollectionParameterKindToString(TypeFilter) : TEXT("all"));
		Result->SetArrayField(TEXT("parameters"), AllParameters);
		Result->SetArrayField(TEXT("scalar_parameters"), ScalarParameters);
		Result->SetArrayField(TEXT("vector_parameters"), VectorParameters);
		Result->SetNumberField(TEXT("parameter_count"), AllParameters.Num());
		Result->SetNumberField(TEXT("scalar_parameter_count"), ScalarParameters.Num());
		Result->SetNumberField(TEXT("vector_parameter_count"), VectorParameters.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionAddScalar(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	double ScalarDefaultValue = 0.0;
	if (!Request.Params->TryGetNumberField(TEXT("default_value"), ScalarDefaultValue))
	{
		Request.Params->TryGetNumberField(TEXT("value"), ScalarDefaultValue);
	}

	auto Task = [CollectionPath, TrimmedParameterName, ScalarDefaultValue]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		if (Collection->GetParameterId(FName(*TrimmedParameterName)).IsValid())
		{
			return MakeFailure(FString::Printf(TEXT("Parameter already exists in collection hierarchy: %s"), *TrimmedParameterName));
		}

		Collection->PreEditChange(nullptr);
		Collection->Modify();

		FCollectionScalarParameter NewParameter;
		NewParameter.ParameterName = FName(*TrimmedParameterName);
		NewParameter.DefaultValue = static_cast<float>(ScalarDefaultValue);
		Collection->ScalarParameters.Add(NewParameter);

		Collection->PostEditChange();
		Collection->MarkPackageDirty();

		const int32 AddedIndex = FindCollectionScalarParameterIndex(Collection, TrimmedParameterName);
		if (!Collection->ScalarParameters.IsValidIndex(AddedIndex))
		{
			return MakeFailure(TEXT("Failed to resolve added scalar parameter after insertion"));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetObjectField(TEXT("parameter"), BuildCollectionScalarParameterJson(Collection->ScalarParameters[AddedIndex], CollectionAssetPath, false));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionAddVector(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	FLinearColor VectorDefaultValue = FLinearColor::Black;
	if (Request.Params->HasField(TEXT("default_value")) || Request.Params->HasField(TEXT("value")))
	{
		FString ParseError;
		if (Request.Params->HasField(TEXT("default_value")))
		{
			if (!TryReadLinearColor(Request.Params, TEXT("default_value"), VectorDefaultValue, ParseError))
			{
				return InvalidParams(Request.Id, ParseError);
			}
		}
		else if (!TryReadLinearColor(Request.Params, TEXT("value"), VectorDefaultValue, ParseError))
		{
			return InvalidParams(Request.Id, ParseError);
		}
	}

	auto Task = [CollectionPath, TrimmedParameterName, VectorDefaultValue]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		if (Collection->GetParameterId(FName(*TrimmedParameterName)).IsValid())
		{
			return MakeFailure(FString::Printf(TEXT("Parameter already exists in collection hierarchy: %s"), *TrimmedParameterName));
		}

		Collection->PreEditChange(nullptr);
		Collection->Modify();

		FCollectionVectorParameter NewParameter;
		NewParameter.ParameterName = FName(*TrimmedParameterName);
		NewParameter.DefaultValue = VectorDefaultValue;
		Collection->VectorParameters.Add(NewParameter);

		Collection->PostEditChange();
		Collection->MarkPackageDirty();

		const int32 AddedIndex = FindCollectionVectorParameterIndex(Collection, TrimmedParameterName);
		if (!Collection->VectorParameters.IsValidIndex(AddedIndex))
		{
			return MakeFailure(TEXT("Failed to resolve added vector parameter after insertion"));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetObjectField(TEXT("parameter"), BuildCollectionVectorParameterJson(Collection->VectorParameters[AddedIndex], CollectionAssetPath, false));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionRemoveParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	ECollectionParameterKind TypeFilter = ECollectionParameterKind::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalCollectionParameterKind(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	auto Task = [CollectionPath, TrimmedParameterName, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		const int32 ScalarIndex = FindCollectionScalarParameterIndex(Collection, TrimmedParameterName);
		const int32 VectorIndex = FindCollectionVectorParameterIndex(Collection, TrimmedParameterName);

		ECollectionParameterKind RemovalKind = ECollectionParameterKind::Unknown;
		int32 RemovalIndex = INDEX_NONE;
		if (bHasTypeFilter)
		{
			RemovalKind = TypeFilter;
			if (TypeFilter == ECollectionParameterKind::Scalar)
			{
				RemovalIndex = ScalarIndex;
			}
			else if (TypeFilter == ECollectionParameterKind::Vector)
			{
				RemovalIndex = VectorIndex;
			}
		}
		else
		{
			if (ScalarIndex != INDEX_NONE && VectorIndex != INDEX_NONE)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter name '%s' exists as both scalar and vector; specify parameter_type"), *TrimmedParameterName));
			}
			if (ScalarIndex != INDEX_NONE)
			{
				RemovalKind = ECollectionParameterKind::Scalar;
				RemovalIndex = ScalarIndex;
			}
			else if (VectorIndex != INDEX_NONE)
			{
				RemovalKind = ECollectionParameterKind::Vector;
				RemovalIndex = VectorIndex;
			}
		}

		if (RemovalIndex == INDEX_NONE)
		{
			const bool bExistsInBaseScalar = Collection->GetScalarParameterByName(FName(*TrimmedParameterName)) != nullptr;
			const bool bExistsInBaseVector = Collection->GetVectorParameterByName(FName(*TrimmedParameterName)) != nullptr;
			if (bExistsInBaseScalar || bExistsInBaseVector)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter '%s' is inherited from a base collection and cannot be removed from this collection"), *TrimmedParameterName));
			}
			return MakeFailure(FString::Printf(TEXT("Parameter not found in collection: %s"), *TrimmedParameterName));
		}

		Collection->PreEditChange(nullptr);
		Collection->Modify();

		TSharedPtr<FJsonObject> RemovedParameter;
		if (RemovalKind == ECollectionParameterKind::Scalar)
		{
			const FCollectionScalarParameter Removed = Collection->ScalarParameters[RemovalIndex];
			RemovedParameter = BuildCollectionScalarParameterJson(Removed, CollectionAssetPath, false);
			Collection->ScalarParameters.RemoveAt(RemovalIndex);
		}
		else
		{
			const FCollectionVectorParameter Removed = Collection->VectorParameters[RemovalIndex];
			RemovedParameter = BuildCollectionVectorParameterJson(Removed, CollectionAssetPath, false);
			Collection->VectorParameters.RemoveAt(RemovalIndex);
		}

		Collection->PostEditChange();
		Collection->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetStringField(TEXT("parameter_type"), CollectionParameterKindToString(RemovalKind));
		Result->SetObjectField(TEXT("removed_parameter"), RemovedParameter);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionRenameParameter(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	FString OldParameterName;
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), OldParameterName))
	{
		Request.Params->TryGetStringField(TEXT("old_parameter_name"), OldParameterName);
	}
	FString NewParameterName;
	if (!Request.Params->TryGetStringField(TEXT("new_parameter_name"), NewParameterName))
	{
		Request.Params->TryGetStringField(TEXT("new_name"), NewParameterName);
	}

	const FString TrimmedOldName = OldParameterName.TrimStartAndEnd();
	const FString TrimmedNewName = NewParameterName.TrimStartAndEnd();
	if (TrimmedOldName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	if (TrimmedNewName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_parameter_name'"));
	}

	ECollectionParameterKind TypeFilter = ECollectionParameterKind::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalCollectionParameterKind(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	auto Task = [CollectionPath, TrimmedOldName, TrimmedNewName, bHasTypeFilter, TypeFilter]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		const int32 ScalarIndex = FindCollectionScalarParameterIndex(Collection, TrimmedOldName);
		const int32 VectorIndex = FindCollectionVectorParameterIndex(Collection, TrimmedOldName);

		ECollectionParameterKind RenameKind = ECollectionParameterKind::Unknown;
		int32 RenameIndex = INDEX_NONE;
		if (bHasTypeFilter)
		{
			RenameKind = TypeFilter;
			if (TypeFilter == ECollectionParameterKind::Scalar)
			{
				RenameIndex = ScalarIndex;
			}
			else if (TypeFilter == ECollectionParameterKind::Vector)
			{
				RenameIndex = VectorIndex;
			}
		}
		else
		{
			if (ScalarIndex != INDEX_NONE && VectorIndex != INDEX_NONE)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter name '%s' exists as both scalar and vector; specify parameter_type"), *TrimmedOldName));
			}
			if (ScalarIndex != INDEX_NONE)
			{
				RenameKind = ECollectionParameterKind::Scalar;
				RenameIndex = ScalarIndex;
			}
			else if (VectorIndex != INDEX_NONE)
			{
				RenameKind = ECollectionParameterKind::Vector;
				RenameIndex = VectorIndex;
			}
		}

		if (RenameIndex == INDEX_NONE)
		{
			const bool bExistsInBaseScalar = Collection->GetScalarParameterByName(FName(*TrimmedOldName)) != nullptr;
			const bool bExistsInBaseVector = Collection->GetVectorParameterByName(FName(*TrimmedOldName)) != nullptr;
			if (bExistsInBaseScalar || bExistsInBaseVector)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter '%s' is inherited from a base collection and cannot be renamed here"), *TrimmedOldName));
			}
			return MakeFailure(FString::Printf(TEXT("Parameter not found in collection: %s"), *TrimmedOldName));
		}

		if (!TrimmedOldName.Equals(TrimmedNewName, ESearchCase::IgnoreCase) &&
			Collection->GetParameterId(FName(*TrimmedNewName)).IsValid())
		{
			return MakeFailure(FString::Printf(TEXT("A parameter named '%s' already exists in this collection hierarchy"), *TrimmedNewName));
		}

		Collection->PreEditChange(nullptr);
		Collection->Modify();
		if (RenameKind == ECollectionParameterKind::Scalar)
		{
			Collection->ScalarParameters[RenameIndex].ParameterName = FName(*TrimmedNewName);
		}
		else
		{
			Collection->VectorParameters[RenameIndex].ParameterName = FName(*TrimmedNewName);
		}
		Collection->PostEditChange();
		Collection->MarkPackageDirty();

		TSharedPtr<FJsonObject> UpdatedParameter;
		if (RenameKind == ECollectionParameterKind::Scalar)
		{
			const int32 UpdatedIndex = FindCollectionScalarParameterIndex(Collection, TrimmedNewName);
			if (!Collection->ScalarParameters.IsValidIndex(UpdatedIndex))
			{
				return MakeFailure(TEXT("Renamed scalar parameter could not be resolved after rename"));
			}
			UpdatedParameter = BuildCollectionScalarParameterJson(Collection->ScalarParameters[UpdatedIndex], CollectionAssetPath, false);
		}
		else
		{
			const int32 UpdatedIndex = FindCollectionVectorParameterIndex(Collection, TrimmedNewName);
			if (!Collection->VectorParameters.IsValidIndex(UpdatedIndex))
			{
				return MakeFailure(TEXT("Renamed vector parameter could not be resolved after rename"));
			}
			UpdatedParameter = BuildCollectionVectorParameterJson(Collection->VectorParameters[UpdatedIndex], CollectionAssetPath, false);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetStringField(TEXT("old_parameter_name"), TrimmedOldName);
		Result->SetStringField(TEXT("new_parameter_name"), UpdatedParameter->GetStringField(TEXT("parameter_name")));
		Result->SetStringField(TEXT("parameter_type"), CollectionParameterKindToString(RenameKind));
		Result->SetObjectField(TEXT("parameter"), UpdatedParameter);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleMaterialCollectionSetDefaultValue(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString CollectionPath;
	if (!Request.Params->TryGetStringField(TEXT("parameter_collection_path"), CollectionPath))
	{
		Request.Params->TryGetStringField(TEXT("asset_path"), CollectionPath);
	}
	if (CollectionPath.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_collection_path'"));
	}

	FString ParameterName;
	if (!Request.Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameter_name'"));
	}
	const FString TrimmedParameterName = ParameterName.TrimStartAndEnd();
	if (TrimmedParameterName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'parameter_name' cannot be empty"));
	}

	ECollectionParameterKind TypeFilter = ECollectionParameterKind::Unknown;
	bool bHasTypeFilter = false;
	FString TypeError;
	if (!ParseOptionalCollectionParameterKind(Request.Params, TypeFilter, bHasTypeFilter, TypeError))
	{
		return InvalidParams(Request.Id, TypeError);
	}

	const bool bHasScalarValueField = Request.Params->HasField(TEXT("value")) || Request.Params->HasField(TEXT("default_value"));
	double ScalarValue = 0.0;
	bool bHasScalarValue = false;
	if (Request.Params->TryGetNumberField(TEXT("value"), ScalarValue) || Request.Params->TryGetNumberField(TEXT("default_value"), ScalarValue))
	{
		bHasScalarValue = true;
	}

	FLinearColor VectorValue = FLinearColor::Black;
	bool bHasVectorValue = false;
	if (Request.Params->HasField(TEXT("value")))
	{
		FString ParseError;
		if (TryReadLinearColor(Request.Params, TEXT("value"), VectorValue, ParseError))
		{
			bHasVectorValue = true;
		}
	}
	if (!bHasVectorValue && Request.Params->HasField(TEXT("default_value")))
	{
		FString ParseError;
		if (TryReadLinearColor(Request.Params, TEXT("default_value"), VectorValue, ParseError))
		{
			bHasVectorValue = true;
		}
	}

	auto Task = [CollectionPath, TrimmedParameterName, bHasTypeFilter, TypeFilter, bHasScalarValueField, bHasScalarValue, ScalarValue, bHasVectorValue, VectorValue]() -> TSharedPtr<FJsonObject>
	{
		FString CollectionAssetPath;
		FString Error;
		UMaterialParameterCollection* Collection = ResolveMaterialParameterCollectionAsset(CollectionPath, CollectionAssetPath, Error);
		if (!Collection)
		{
			return MakeFailure(Error);
		}

		const int32 DirectScalarIndex = FindCollectionScalarParameterIndex(Collection, TrimmedParameterName);
		const int32 DirectVectorIndex = FindCollectionVectorParameterIndex(Collection, TrimmedParameterName);
		const bool bScalarExists = Collection->GetScalarParameterByName(FName(*TrimmedParameterName)) != nullptr;
		const bool bVectorExists = Collection->GetVectorParameterByName(FName(*TrimmedParameterName)) != nullptr;
		const bool bDirectScalarExists = DirectScalarIndex != INDEX_NONE;
		const bool bDirectVectorExists = DirectVectorIndex != INDEX_NONE;

		ECollectionParameterKind TargetKind = ECollectionParameterKind::Unknown;
		if (bHasTypeFilter)
		{
			TargetKind = TypeFilter;
		}
		else
		{
			if (bDirectScalarExists && bDirectVectorExists)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter '%s' exists as both scalar and vector; specify parameter_type"), *TrimmedParameterName));
			}
			if (bDirectScalarExists)
			{
				TargetKind = ECollectionParameterKind::Scalar;
			}
			else if (bDirectVectorExists)
			{
				TargetKind = ECollectionParameterKind::Vector;
			}
		}

		if (TargetKind == ECollectionParameterKind::Unknown)
		{
			if (bScalarExists || bVectorExists)
			{
				return MakeFailure(FString::Printf(TEXT("Parameter '%s' is inherited from a base collection; setting inherited overrides is not supported by this tool"), *TrimmedParameterName));
			}
			return MakeFailure(FString::Printf(TEXT("Parameter not found in collection: %s"), *TrimmedParameterName));
		}

		if (TargetKind == ECollectionParameterKind::Scalar && !bDirectScalarExists)
		{
			if (bScalarExists)
			{
				return MakeFailure(FString::Printf(TEXT("Scalar parameter '%s' is inherited from a base collection; setting inherited overrides is not supported by this tool"), *TrimmedParameterName));
			}
			return MakeFailure(FString::Printf(TEXT("Scalar parameter not found in collection: %s"), *TrimmedParameterName));
		}
		if (TargetKind == ECollectionParameterKind::Vector && !bDirectVectorExists)
		{
			if (bVectorExists)
			{
				return MakeFailure(FString::Printf(TEXT("Vector parameter '%s' is inherited from a base collection; setting inherited overrides is not supported by this tool"), *TrimmedParameterName));
			}
			return MakeFailure(FString::Printf(TEXT("Vector parameter not found in collection: %s"), *TrimmedParameterName));
		}

		if (TargetKind == ECollectionParameterKind::Scalar)
		{
			if (!bHasScalarValueField || !bHasScalarValue)
			{
				return MakeFailure(TEXT("Scalar default value requires numeric 'value' (or 'default_value')"));
			}
		}
		else if (!bHasVectorValue)
		{
			return MakeFailure(TEXT("Vector default value requires a color object/string in 'value' (or 'default_value')"));
		}

		Collection->PreEditChange(nullptr);
		Collection->Modify();

		bool bChanged = false;
		if (TargetKind == ECollectionParameterKind::Scalar)
		{
			FCollectionScalarParameter& ScalarParameter = Collection->ScalarParameters[DirectScalarIndex];
			const float NewValue = static_cast<float>(ScalarValue);
			bChanged = !FMath::IsNearlyEqual(ScalarParameter.DefaultValue, NewValue);
			ScalarParameter.DefaultValue = NewValue;
		}
		else
		{
			FCollectionVectorParameter& VectorParameter = Collection->VectorParameters[DirectVectorIndex];
			bChanged = !VectorParameter.DefaultValue.Equals(VectorValue);
			VectorParameter.DefaultValue = VectorValue;
		}

		Collection->PostEditChange();
		Collection->MarkPackageDirty();

		if (!bChanged)
		{
			return MakeFailure(FString::Printf(TEXT("Failed to set default value for parameter: %s"), *TrimmedParameterName));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("parameter_collection_path"), CollectionAssetPath);
		Result->SetStringField(TEXT("parameter_name"), TrimmedParameterName);
		Result->SetStringField(TEXT("parameter_type"), CollectionParameterKindToString(TargetKind));
		if (TargetKind == ECollectionParameterKind::Scalar)
		{
			const float UpdatedValue = Collection->ScalarParameters[DirectScalarIndex].DefaultValue;
			Result->SetBoolField(TEXT("parameter_found"), true);
			Result->SetNumberField(TEXT("default_value"), UpdatedValue);
		}
		else
		{
			const FLinearColor UpdatedValue = Collection->VectorParameters[DirectVectorIndex].DefaultValue;
			Result->SetBoolField(TEXT("parameter_found"), true);
			Result->SetObjectField(TEXT("default_value"), BuildColorJson(UpdatedValue));
		}
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleFindReferences(const FMCPRequest& Request)
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

	FString SymbolTypeString = TEXT("node");
	Request.Params->TryGetStringField(TEXT("symbol_type"), SymbolTypeString);

	FString SymbolName;
	Request.Params->TryGetStringField(TEXT("symbol_name"), SymbolName);
	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);
	FString FunctionPath;
	Request.Params->TryGetStringField(TEXT("function_path"), FunctionPath);

	auto Task = [AssetPath, SymbolTypeString, SymbolName, NodeId, FunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		EMaterialSymbolType SymbolType = EMaterialSymbolType::Unknown;
		if (!ParseMaterialSymbolType(SymbolTypeString, SymbolType))
		{
			return MakeFailure(FString::Printf(TEXT("Unsupported symbol_type '%s'. Supported: parameter, function_call, node"), *SymbolTypeString));
		}

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		TArray<UMaterialExpression*> TargetNodes;
		if (SymbolType == EMaterialSymbolType::Parameter)
		{
			if (!Context.Material)
			{
				return MakeFailure(TEXT("symbol_type=parameter is only supported for material assets"));
			}

			const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
			const FString TrimmedSymbolName = SymbolName.TrimStartAndEnd();
			if (!TrimmedNodeId.IsEmpty())
			{
				UMaterialExpression* TargetParameter = FindNodeById(Context, TrimmedNodeId);
				if (!TargetParameter)
				{
					return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId));
				}
				if (!IsSupportedParameterExpression(TargetParameter))
				{
					return MakeFailure(FString::Printf(TEXT("Node is not a supported parameter expression: %s"), *TrimmedNodeId));
				}
				TargetNodes.Add(TargetParameter);
			}
			else
			{
				GatherParameterMatchesByName(Context, TrimmedSymbolName, false, EMaterialParameterNodeType::Unknown, TargetNodes);
				if (TargetNodes.Num() == 0)
				{
					return MakeFailure(FString::Printf(TEXT("Parameter not found: %s"), *TrimmedSymbolName));
				}
			}
		}
		else if (SymbolType == EMaterialSymbolType::FunctionCall)
		{
			const FString TrimmedFunctionPath = NormalizeAssetPath(FunctionPath);
			const FString TrimmedSymbolName = SymbolName.TrimStartAndEnd();
			const FString TrimmedNodeId = NodeId.TrimStartAndEnd();

			if (!TrimmedNodeId.IsEmpty())
			{
				UMaterialExpression* TargetNode = FindNodeById(Context, TrimmedNodeId);
				if (!TargetNode)
				{
					return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId));
				}
				UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(TargetNode);
				if (!FunctionCall)
				{
					return MakeFailure(FString::Printf(TEXT("Node is not a function call: %s"), *TrimmedNodeId));
				}
				TargetNodes.Add(FunctionCall);
			}
			else
			{
				for (UMaterialExpression* Expression : Expressions)
				{
					UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
					if (!FunctionCall)
					{
						continue;
					}

					if (!TrimmedFunctionPath.IsEmpty())
					{
						const FString CurrentFunctionPath = FunctionCall->MaterialFunction
							? NormalizeAssetPath(FunctionCall->MaterialFunction->GetPathName())
							: FString();
						if (!CurrentFunctionPath.Equals(TrimmedFunctionPath, ESearchCase::IgnoreCase))
						{
							continue;
						}
					}

					if (!TrimmedSymbolName.IsEmpty())
					{
						const FString CurrentFunctionName = FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : FString();
						if (!CurrentFunctionName.Equals(TrimmedSymbolName, ESearchCase::IgnoreCase))
						{
							continue;
						}
					}

					TargetNodes.Add(FunctionCall);
				}

				if (TargetNodes.Num() == 0)
				{
					return MakeFailure(TEXT("No function call nodes matched the provided filter"));
				}
			}
		}
		else
		{
			const FString TargetNodeId = !NodeId.TrimStartAndEnd().IsEmpty() ? NodeId.TrimStartAndEnd() : SymbolName.TrimStartAndEnd();
			if (TargetNodeId.IsEmpty())
			{
				return MakeFailure(TEXT("symbol_type=node requires node_id (or symbol_name as node id)"));
			}

			UMaterialExpression* Node = FindNodeById(Context, TargetNodeId);
			if (!Node)
			{
				return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *TargetNodeId));
			}
			TargetNodes.Add(Node);
		}

		TArray<TSharedPtr<FJsonValue>> ReferencesJson;
		TArray<TSharedPtr<FJsonValue>> TargetNodesJson;

		auto AddReferenceObject = [&ReferencesJson](
			const FString& ReferenceType,
			UMaterialExpression* SourceNode,
			const int32 SourceOutputIndex,
			UMaterialExpression* TargetNode,
			const int32 TargetInputIndex,
			const FString& OutputName,
			const FString& InputName)
		{
			TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
			RefObj->SetStringField(TEXT("reference_type"), ReferenceType);
			RefObj->SetStringField(TEXT("from_node_id"), SourceNode ? GetNodeId(SourceNode) : FString());
			RefObj->SetStringField(TEXT("from_node_name"), SourceNode ? SourceNode->GetName() : FString());
			RefObj->SetNumberField(TEXT("from_output_index"), SourceOutputIndex);
			RefObj->SetStringField(TEXT("from_output_name"), OutputName);
			RefObj->SetStringField(TEXT("to_node_id"), TargetNode ? GetNodeId(TargetNode) : FString());
			RefObj->SetStringField(TEXT("to_node_name"), TargetNode ? TargetNode->GetName() : FString());
			RefObj->SetNumberField(TEXT("to_input_index"), TargetInputIndex);
			RefObj->SetStringField(TEXT("to_input_name"), InputName);
			ReferencesJson.Add(MakeShared<FJsonValueObject>(RefObj));
		};

		for (UMaterialExpression* TargetNode : TargetNodes)
		{
			if (!TargetNode)
			{
				continue;
			}
			TargetNodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(TargetNode)));

			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = TargetNode->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}
				if (!Input->Expression)
				{
					continue;
				}

				UMaterialExpression* SourceNode = Input->Expression;
				const int32 SourceOutputIndex = Input->OutputIndex;
				const FString OutputName = GetOutputPinDisplayName(SourceNode, SourceOutputIndex, SourceNode->GetOutput(SourceOutputIndex));
				const FString InputName = GetInputPinDisplayName(TargetNode, InputIndex, Input);
				AddReferenceObject(TEXT("input_link"), SourceNode, SourceOutputIndex, TargetNode, InputIndex, OutputName, InputName);
			}

			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression || Expression == TargetNode)
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
					if (Input->Expression != TargetNode)
					{
						continue;
					}

					const int32 SourceOutputIndex = Input->OutputIndex;
					const FString OutputName = GetOutputPinDisplayName(TargetNode, SourceOutputIndex, TargetNode->GetOutput(SourceOutputIndex));
					const FString InputName = GetInputPinDisplayName(Expression, InputIndex, Input);
					AddReferenceObject(TEXT("output_link"), TargetNode, SourceOutputIndex, Expression, InputIndex, OutputName, InputName);
				}
			}

			if (Context.Material)
			{
				TArray<EMaterialProperty> OutputProperties;
				AddPhase4MaterialOutputProperties(OutputProperties);
				for (const EMaterialProperty OutputProperty : OutputProperties)
				{
					FExpressionInput* PropertyInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
					if (!PropertyInput || PropertyInput->Expression != TargetNode)
					{
						continue;
					}

					TSharedPtr<FJsonObject> OutputRef = MakeShared<FJsonObject>();
					OutputRef->SetStringField(TEXT("reference_type"), TEXT("material_output"));
					OutputRef->SetStringField(TEXT("from_node_id"), GetNodeId(TargetNode));
					OutputRef->SetStringField(TEXT("from_node_name"), TargetNode->GetName());
					OutputRef->SetNumberField(TEXT("from_output_index"), PropertyInput->OutputIndex);
					OutputRef->SetStringField(TEXT("from_output_name"), GetOutputPinDisplayName(TargetNode, PropertyInput->OutputIndex, TargetNode->GetOutput(PropertyInput->OutputIndex)));
					OutputRef->SetStringField(TEXT("output_name"), MaterialPropertyToOutputName(OutputProperty));
					OutputRef->SetNumberField(TEXT("output_property"), static_cast<int32>(OutputProperty));
					ReferencesJson.Add(MakeShared<FJsonValueObject>(OutputRef));
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("symbol_type"), MaterialSymbolTypeToString(SymbolType));
		Result->SetArrayField(TEXT("target_nodes"), TargetNodesJson);
		Result->SetNumberField(TEXT("target_count"), TargetNodesJson.Num());
		Result->SetArrayField(TEXT("references"), ReferencesJson);
		Result->SetNumberField(TEXT("count"), ReferencesJson.Num());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRenameSymbol(const FMCPRequest& Request)
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

	FString SymbolTypeString = TEXT("parameter");
	Request.Params->TryGetStringField(TEXT("symbol_type"), SymbolTypeString);
	FString NewName;
	if (!Request.Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_name'"));
	}
	const FString TrimmedNewName = NewName.TrimStartAndEnd();
	if (TrimmedNewName.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'new_name' cannot be empty"));
	}

	FString OldName;
	Request.Params->TryGetStringField(TEXT("old_name"), OldName);
	if (OldName.TrimStartAndEnd().IsEmpty())
	{
		Request.Params->TryGetStringField(TEXT("symbol_name"), OldName);
	}
	FString NodeId;
	Request.Params->TryGetStringField(TEXT("node_id"), NodeId);

	bool bRenameAllMatches = false;
	Request.Params->TryGetBoolField(TEXT("rename_all_matches"), bRenameAllMatches);

	FString NewFunctionPath = TrimmedNewName;
	Request.Params->TryGetStringField(TEXT("new_function_path"), NewFunctionPath);
	NewFunctionPath = NewFunctionPath.TrimStartAndEnd();

	auto Task = [AssetPath, SymbolTypeString, TrimmedNewName, OldName, NodeId, bRenameAllMatches, NewFunctionPath]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		EMaterialSymbolType SymbolType = EMaterialSymbolType::Unknown;
		if (!ParseMaterialSymbolType(SymbolTypeString, SymbolType))
		{
			return MakeFailure(FString::Printf(TEXT("Unsupported symbol_type '%s'. Supported: parameter, function_call, node"), *SymbolTypeString));
		}

		if (SymbolType == EMaterialSymbolType::Parameter)
		{
			if (!Context.Material)
			{
				return MakeFailure(TEXT("symbol_type=parameter is only supported for material assets"));
			}

			const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
			const FString TrimmedOldName = OldName.TrimStartAndEnd();

			TArray<UMaterialExpression*> Targets;
			if (!TrimmedNodeId.IsEmpty())
			{
				UMaterialExpression* TargetExpression = FindNodeById(Context, TrimmedNodeId);
				if (!TargetExpression)
				{
					return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *TrimmedNodeId));
				}
				if (!IsSupportedParameterExpression(TargetExpression))
				{
					return MakeFailure(FString::Printf(TEXT("Node is not a supported parameter expression: %s"), *TrimmedNodeId));
				}
				Targets.Add(TargetExpression);
			}
			else
			{
				GatherParameterMatchesByName(Context, TrimmedOldName, false, EMaterialParameterNodeType::Unknown, Targets);
				if (Targets.Num() == 0)
				{
					return MakeFailure(FString::Printf(TEXT("Parameter not found: %s"), *TrimmedOldName));
				}
				if (Targets.Num() > 1 && !bRenameAllMatches)
				{
					return MakeFailure(FString::Printf(TEXT("Parameter name is ambiguous (%d matches). Provide node_id or set rename_all_matches=true."), Targets.Num()));
				}
			}

			TArray<UMaterialExpression*> AllParameters;
			GatherParameterExpressions(Context, AllParameters);
			TSet<UMaterialExpression*> TargetSet;
			for (UMaterialExpression* Target : Targets)
			{
				if (Target)
				{
					TargetSet.Add(Target);
				}
			}

			for (UMaterialExpression* Existing : AllParameters)
			{
				if (!Existing || TargetSet.Contains(Existing))
				{
					continue;
				}
				if (GetParameterExpressionName(Existing).ToString().Equals(TrimmedNewName, ESearchCase::IgnoreCase))
				{
					return MakeFailure(FString::Printf(TEXT("Parameter name already exists: %s"), *TrimmedNewName));
				}
			}

			int32 ChangedCount = 0;
			TArray<TSharedPtr<FJsonValue>> RenamedJson;
			for (UMaterialExpression* Target : Targets)
			{
				if (!Target)
				{
					continue;
				}

				const FName ExistingName = GetParameterExpressionName(Target);
				const bool bChanged = !ExistingName.ToString().Equals(TrimmedNewName, ESearchCase::IgnoreCase);
				if (bChanged)
				{
					Target->Modify();
					if (!SetParameterExpressionName(Target, FName(*TrimmedNewName)))
					{
						return MakeFailure(TEXT("Target node does not expose a parameter name"));
					}
					++ChangedCount;
				}

				TSharedPtr<FJsonObject> RenamedObj = MakeShared<FJsonObject>();
				RenamedObj->SetStringField(TEXT("node_id"), GetNodeId(Target));
				RenamedObj->SetStringField(TEXT("old_name"), ExistingName.ToString());
				RenamedObj->SetStringField(TEXT("new_name"), GetParameterExpressionName(Target).ToString());
				RenamedObj->SetBoolField(TEXT("changed"), bChanged);
				RenamedObj->SetObjectField(TEXT("parameter"), BuildParameterJson(Target));
				RenamedJson.Add(MakeShared<FJsonValueObject>(RenamedObj));
			}

			if (ChangedCount > 0)
			{
				Context.MarkDirty();
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
			Result->SetStringField(TEXT("symbol_type"), TEXT("parameter"));
			Result->SetStringField(TEXT("new_name"), TrimmedNewName);
			Result->SetBoolField(TEXT("rename_all_matches"), bRenameAllMatches);
			Result->SetBoolField(TEXT("changed"), ChangedCount > 0);
			Result->SetNumberField(TEXT("changed_count"), ChangedCount);
			Result->SetArrayField(TEXT("renamed"), RenamedJson);
			return Result;
		}

		if (SymbolType == EMaterialSymbolType::FunctionCall)
		{
			const FString ReplacementFunctionPath = NormalizeAssetPath(NewFunctionPath);
			if (!FPackageName::IsValidLongPackageName(ReplacementFunctionPath))
			{
				return MakeFailure(FString::Printf(TEXT("Invalid new_function_path: %s"), *NewFunctionPath));
			}

			UMaterialFunctionInterface* ReplacementFunction = LoadAssetAs<UMaterialFunctionInterface>(ReplacementFunctionPath);
			if (!ReplacementFunction)
			{
				return MakeFailure(FString::Printf(TEXT("Replacement function not found: %s"), *ReplacementFunctionPath));
			}

			const FString TrimmedNodeId = NodeId.TrimStartAndEnd();
			const FString TrimmedOldName = OldName.TrimStartAndEnd();

			TArray<UMaterialExpression*> Expressions;
			TArray<UMaterialExpressionComment*> Comments;
			GatherGraphNodes(Context, Expressions, Comments);

			TArray<TSharedPtr<FJsonValue>> UpdatedNodes;
			int32 ChangedCount = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
				if (!FunctionCall)
				{
					continue;
				}

				if (!TrimmedNodeId.IsEmpty() && !GetNodeId(FunctionCall).Equals(TrimmedNodeId, ESearchCase::IgnoreCase))
				{
					continue;
				}

				if (!TrimmedOldName.IsEmpty())
				{
					const FString CurrentFunctionPath = FunctionCall->MaterialFunction ? NormalizeAssetPath(FunctionCall->MaterialFunction->GetPathName()) : FString();
					const FString CurrentFunctionName = FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : FString();
					if (!CurrentFunctionPath.Equals(NormalizeAssetPath(TrimmedOldName), ESearchCase::IgnoreCase)
						&& !CurrentFunctionName.Equals(TrimmedOldName, ESearchCase::IgnoreCase))
					{
						continue;
					}
				}

				const FString OldFunctionPath = FunctionCall->MaterialFunction ? NormalizeAssetPath(FunctionCall->MaterialFunction->GetPathName()) : FString();
				if (OldFunctionPath.Equals(ReplacementFunctionPath, ESearchCase::IgnoreCase))
				{
					continue;
				}

				FunctionCall->Modify();
				const bool bSetResult = FunctionCall->SetMaterialFunction(ReplacementFunction);
				if (!bSetResult)
				{
					return MakeFailure(FString::Printf(TEXT("Failed to replace function call on node: %s"), *GetNodeId(FunctionCall)));
				}

				++ChangedCount;
				TSharedPtr<FJsonObject> NodeObj = BuildNodeJson(FunctionCall);
				NodeObj->SetStringField(TEXT("old_function_path"), OldFunctionPath);
				NodeObj->SetStringField(TEXT("new_function_path"), ReplacementFunctionPath);
				UpdatedNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}

			if (ChangedCount > 0)
			{
				Context.MarkDirty();
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
			Result->SetStringField(TEXT("symbol_type"), TEXT("function_call"));
			Result->SetStringField(TEXT("new_function_path"), ReplacementFunctionPath);
			Result->SetBoolField(TEXT("changed"), ChangedCount > 0);
			Result->SetNumberField(TEXT("changed_count"), ChangedCount);
			Result->SetArrayField(TEXT("updated_nodes"), UpdatedNodes);
			return Result;
		}

		const FString TargetNodeId = !NodeId.TrimStartAndEnd().IsEmpty() ? NodeId.TrimStartAndEnd() : OldName.TrimStartAndEnd();
		if (TargetNodeId.IsEmpty())
		{
			return MakeFailure(TEXT("symbol_type=node requires node_id (or old_name)"));
		}

		UMaterialExpression* TargetNode = FindNodeById(Context, TargetNodeId);
		if (!TargetNode)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *TargetNodeId));
		}

		const FString OldNodeLabel = TargetNode->IsA<UMaterialExpressionComment>()
			? Cast<UMaterialExpressionComment>(TargetNode)->Text
			: TargetNode->Desc;

		TargetNode->Modify();
		if (UMaterialExpressionComment* CommentNode = Cast<UMaterialExpressionComment>(TargetNode))
		{
			CommentNode->Text = TrimmedNewName;
		}
		else
		{
			TargetNode->Desc = TrimmedNewName;
		}
		Context.MarkDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("symbol_type"), TEXT("node"));
		Result->SetStringField(TEXT("node_id"), GetNodeId(TargetNode));
		Result->SetStringField(TEXT("old_name"), OldNodeLabel);
		Result->SetStringField(TEXT("new_name"), TrimmedNewName);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(TargetNode));
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleReplaceFunctionCalls(const FMCPRequest& Request)
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

	FString NewFunctionPath;
	if (!Request.Params->TryGetStringField(TEXT("new_function_path"), NewFunctionPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_function_path'"));
	}
	NewFunctionPath = NewFunctionPath.TrimStartAndEnd();
	if (NewFunctionPath.IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("Parameter 'new_function_path' cannot be empty"));
	}

	FString OldFunctionPath;
	Request.Params->TryGetStringField(TEXT("old_function_path"), OldFunctionPath);
	const FString TrimmedOldFunctionPath = OldFunctionPath.TrimStartAndEnd();

	FString OldFunctionName;
	Request.Params->TryGetStringField(TEXT("old_function_name"), OldFunctionName);
	const FString TrimmedOldFunctionName = OldFunctionName.TrimStartAndEnd();

	auto Task = [AssetPath, NewFunctionPath, TrimmedOldFunctionPath, TrimmedOldFunctionName]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}

		const FString ReplacementPath = NormalizeAssetPath(NewFunctionPath);
		if (!FPackageName::IsValidLongPackageName(ReplacementPath))
		{
			return MakeFailure(FString::Printf(TEXT("Invalid new_function_path: %s"), *NewFunctionPath));
		}

		UMaterialFunctionInterface* ReplacementFunction = LoadAssetAs<UMaterialFunctionInterface>(ReplacementPath);
		if (!ReplacementFunction)
		{
			return MakeFailure(FString::Printf(TEXT("Replacement function not found: %s"), *ReplacementPath));
		}

		const FString FilterPath = NormalizeAssetPath(TrimmedOldFunctionPath);
		const bool bFilterByPath = !FilterPath.IsEmpty();
		const bool bFilterByName = !TrimmedOldFunctionName.IsEmpty();

		TArray<UMaterialExpression*> Expressions;
		TArray<UMaterialExpressionComment*> Comments;
		GatherGraphNodes(Context, Expressions, Comments);

		int32 ChangedCount = 0;
		TArray<TSharedPtr<FJsonValue>> UpdatedNodes;
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (!FunctionCall)
			{
				continue;
			}

			const FString ExistingPath = FunctionCall->MaterialFunction ? NormalizeAssetPath(FunctionCall->MaterialFunction->GetPathName()) : FString();
			const FString ExistingName = FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : FString();

			if (bFilterByPath && !ExistingPath.Equals(FilterPath, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (bFilterByName && !ExistingName.Equals(TrimmedOldFunctionName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ExistingPath.Equals(ReplacementPath, ESearchCase::IgnoreCase))
			{
				continue;
			}

			FunctionCall->Modify();
			if (!FunctionCall->SetMaterialFunction(ReplacementFunction))
			{
				return MakeFailure(FString::Printf(TEXT("Failed to replace function call on node: %s"), *GetNodeId(FunctionCall)));
			}

			++ChangedCount;
			TSharedPtr<FJsonObject> UpdatedNode = BuildNodeJson(FunctionCall);
			UpdatedNode->SetStringField(TEXT("old_function_path"), ExistingPath);
			UpdatedNode->SetStringField(TEXT("new_function_path"), ReplacementPath);
			UpdatedNodes.Add(MakeShared<FJsonValueObject>(UpdatedNode));
		}

		if (ChangedCount > 0)
		{
			Context.MarkDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("new_function_path"), ReplacementPath);
		Result->SetStringField(TEXT("old_function_path_filter"), FilterPath);
		Result->SetStringField(TEXT("old_function_name_filter"), TrimmedOldFunctionName);
		Result->SetBoolField(TEXT("changed"), ChangedCount > 0);
		Result->SetNumberField(TEXT("changed_count"), ChangedCount);
		Result->SetArrayField(TEXT("updated_nodes"), UpdatedNodes);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRemoveUnusedParameters(const FMCPRequest& Request)
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

	bool bDryRun = false;
	Request.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	auto Task = [AssetPath, bDryRun]() -> TSharedPtr<FJsonObject>
	{
		FMaterialGraphContext Context;
		FString Error;
		if (!ResolveGraphContext(AssetPath, Context, Error))
		{
			return MakeFailure(Error);
		}
		if (!Context.Material)
		{
			return MakeFailure(TEXT("material/remove_unused_parameters only supports UMaterial assets"));
		}

		TArray<UMaterialExpression*> Parameters;
		GatherParameterExpressions(Context, Parameters);

		TArray<EMaterialProperty> OutputProperties;
		AddPhase4MaterialOutputProperties(OutputProperties);

		TArray<UMaterialExpression*> Candidates;
		for (UMaterialExpression* Parameter : Parameters)
		{
			if (!Parameter)
			{
				continue;
			}

			bool bUsed = false;
			for (int32 OutputIndex = 0;; ++OutputIndex)
			{
				if (!Parameter->GetOutput(OutputIndex))
				{
					break;
				}

				if (CountOutputPinLinks(Context, Parameter, OutputIndex) > 0)
				{
					bUsed = true;
					break;
				}
			}

			if (!bUsed)
			{
				for (const EMaterialProperty OutputProperty : OutputProperties)
				{
					FExpressionInput* MaterialInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
					if (MaterialInput && MaterialInput->Expression == Parameter)
					{
						bUsed = true;
						break;
					}
				}
			}

			if (!bUsed)
			{
				Candidates.Add(Parameter);
			}
		}

		TArray<TSharedPtr<FJsonValue>> RemovedJson;
		RemovedJson.Reserve(Candidates.Num());
		if (!bDryRun)
		{
			for (UMaterialExpression* Candidate : Candidates)
			{
				if (!Candidate)
				{
					continue;
				}
				RemovedJson.Add(MakeShared<FJsonValueObject>(BuildParameterJson(Candidate)));
				UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, Candidate);
			}
			if (RemovedJson.Num() > 0)
			{
				Context.MarkDirty();
			}
		}
		else
		{
			for (UMaterialExpression* Candidate : Candidates)
			{
				if (Candidate)
				{
					RemovedJson.Add(MakeShared<FJsonValueObject>(BuildParameterJson(Candidate)));
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetNumberField(TEXT("candidate_count"), Candidates.Num());
		Result->SetNumberField(TEXT("removed_count"), RemovedJson.Num());
		Result->SetArrayField(TEXT("removed_parameters"), RemovedJson);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleRemoveOrphanNodes(const FMCPRequest& Request)
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

	bool bDryRun = false;
	Request.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bIncludeComments = false;
	Request.Params->TryGetBoolField(TEXT("include_comments"), bIncludeComments);

	auto Task = [AssetPath, bDryRun, bIncludeComments]() -> TSharedPtr<FJsonObject>
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

		TSet<UMaterialExpression*> Reachable;
		TFunction<void(UMaterialExpression*)> MarkReachable;
		MarkReachable = [&MarkReachable, &Reachable](UMaterialExpression* Node)
		{
			if (!Node || Reachable.Contains(Node))
			{
				return;
			}

			Reachable.Add(Node);
			for (int32 InputIndex = 0;; ++InputIndex)
			{
				FExpressionInput* Input = Node->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}
				if (Input->Expression)
				{
					MarkReachable(Input->Expression);
				}
			}
		};

		if (Context.Material)
		{
			TArray<EMaterialProperty> OutputProperties;
			AddPhase4MaterialOutputProperties(OutputProperties);
			for (const EMaterialProperty OutputProperty : OutputProperties)
			{
				FExpressionInput* OutputInput = Context.Material->GetExpressionInputForProperty(OutputProperty);
				if (OutputInput && OutputInput->Expression)
				{
					MarkReachable(OutputInput->Expression);
				}
			}
		}
		else if (Context.MaterialFunction)
		{
			TArray<UMaterialExpressionFunctionInput*> FunctionInputs;
			TArray<UMaterialExpressionFunctionOutput*> FunctionOutputs;
			GatherMaterialFunctionIONodes(Context.MaterialFunction, FunctionInputs, FunctionOutputs);
			for (UMaterialExpressionFunctionOutput* OutputNode : FunctionOutputs)
			{
				if (!OutputNode)
				{
					continue;
				}
				MarkReachable(OutputNode);
				if (OutputNode->A.Expression)
				{
					MarkReachable(OutputNode->A.Expression);
				}
			}
		}

		TArray<UMaterialExpression*> OrphanExpressions;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression || Reachable.Contains(Expression))
			{
				continue;
			}
			OrphanExpressions.Add(Expression);
		}

		TArray<UMaterialExpressionComment*> OrphanComments;
		if (bIncludeComments)
		{
			for (UMaterialExpressionComment* Comment : Comments)
			{
				if (Comment)
				{
					OrphanComments.Add(Comment);
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> RemovedNodesJson;
		RemovedNodesJson.Reserve(OrphanExpressions.Num() + OrphanComments.Num());

		for (UMaterialExpression* OrphanExpression : OrphanExpressions)
		{
			if (OrphanExpression)
			{
				RemovedNodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(OrphanExpression)));
			}
		}
		for (UMaterialExpressionComment* OrphanComment : OrphanComments)
		{
			if (OrphanComment)
			{
				RemovedNodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(OrphanComment)));
			}
		}

		if (!bDryRun)
		{
			for (UMaterialExpression* OrphanExpression : OrphanExpressions)
			{
				if (!OrphanExpression)
				{
					continue;
				}
				if (Context.Material)
				{
					UMaterialEditingLibrary::DeleteMaterialExpression(Context.Material, OrphanExpression);
				}
				else if (Context.MaterialFunction)
				{
					UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Context.MaterialFunction, OrphanExpression);
				}
			}

			for (UMaterialExpressionComment* OrphanComment : OrphanComments)
			{
				if (!OrphanComment)
				{
					continue;
				}
				if (Context.Material)
				{
					Context.Material->GetExpressionCollection().RemoveComment(OrphanComment);
				}
				else if (Context.MaterialFunction)
				{
					Context.MaterialFunction->GetExpressionCollection().RemoveComment(OrphanComment);
				}
				OrphanComment->MarkAsGarbage();
			}

			if (RemovedNodesJson.Num() > 0)
			{
				Context.MarkDirty();
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
		Result->SetStringField(TEXT("asset_type"), Context.Material ? TEXT("material") : TEXT("material_function"));
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("include_comments"), bIncludeComments);
		Result->SetNumberField(TEXT("candidate_expression_count"), OrphanExpressions.Num());
		Result->SetNumberField(TEXT("candidate_comment_count"), OrphanComments.Num());
		Result->SetNumberField(TEXT("removed_count"), RemovedNodesJson.Num());
		Result->SetArrayField(TEXT("removed_nodes"), RemovedNodesJson);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCompileMaterial(const FMCPRequest& Request)
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

	int32 MaxMessages = INDEX_NONE;
	double MaxMessagesNumber = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("max_messages"), MaxMessagesNumber))
	{
		if (MaxMessagesNumber < 0.0)
		{
			return InvalidParams(Request.Id, TEXT("'max_messages' must be >= 0"));
		}
		MaxMessages = FMath::FloorToInt(MaxMessagesNumber);
	}

	bool bIncludeMessages = true;
	Request.Params->TryGetBoolField(TEXT("include_messages"), bIncludeMessages);

	auto Task = [AssetPath, MaxMessages, bIncludeMessages]() -> TSharedPtr<FJsonObject>
	{
		FString MaterialInterfacePath;
		FString Error;
		UMaterialInterface* MaterialInterface = ResolveMaterialInterfaceAsset(AssetPath, MaterialInterfacePath, Error);
		if (!MaterialInterface)
		{
			return MakeFailure(Error);
		}

		UMaterial* Material = Cast<UMaterial>(MaterialInterface);
		if (!Material)
		{
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface);
			Material = const_cast<UMaterial*>(Instance ? Instance->GetMaterial() : nullptr);
		}
		if (!Material)
		{
			return MakeFailure(TEXT("Unable to resolve a source material for compilation"));
		}

		UMaterialEditingLibrary::RecompileMaterial(Material);

		FMaterialCompileDiagnostics Diagnostics;
		GatherMaterialCompileDiagnostics(Material, Diagnostics);
		const FMaterialStatistics ShaderStats = UMaterialEditingLibrary::GetStatistics(MaterialInterface);

		TArray<TSharedPtr<FJsonValue>> ErrorNodesJson;
		for (const TWeakObjectPtr<UMaterialExpression>& ErrorExpression : Diagnostics.ErrorExpressions)
		{
			if (UMaterialExpression* Node = ErrorExpression.Get())
			{
				ErrorNodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
			}
		}

		TArray<TSharedPtr<FJsonValue>> CompileErrorsJson;
		for (const FString& CompileError : Diagnostics.CompileErrors)
		{
			CompileErrorsJson.Add(MakeShared<FJsonValueString>(CompileError));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), MaterialInterfacePath);
		Result->SetStringField(TEXT("material_path"), NormalizeAssetPath(Material->GetPathName()));
		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetBoolField(TEXT("is_compiling"), Diagnostics.bIsCompiling);
		Result->SetBoolField(TEXT("had_compile_error"), Diagnostics.bHadCompileError);
		Result->SetNumberField(TEXT("num_errors"), Diagnostics.CompileErrors.Num());
		Result->SetArrayField(TEXT("compile_errors"), CompileErrorsJson);
		Result->SetArrayField(TEXT("error_nodes"), ErrorNodesJson);
		Result->SetNumberField(TEXT("error_node_count"), ErrorNodesJson.Num());
		Result->SetNumberField(TEXT("num_vertex_shader_instructions"), ShaderStats.NumVertexShaderInstructions);
		Result->SetNumberField(TEXT("num_pixel_shader_instructions"), ShaderStats.NumPixelShaderInstructions);
		Result->SetNumberField(TEXT("num_samplers"), ShaderStats.NumSamplers);
		Result->SetNumberField(TEXT("num_vertex_texture_samples"), ShaderStats.NumVertexTextureSamples);
		Result->SetNumberField(TEXT("num_pixel_texture_samples"), ShaderStats.NumPixelTextureSamples);
		Result->SetNumberField(TEXT("num_virtual_texture_samples"), ShaderStats.NumVirtualTextureSamples);
		Result->SetNumberField(TEXT("num_uv_scalars"), ShaderStats.NumUVScalars);
		Result->SetNumberField(TEXT("num_interpolator_scalars"), ShaderStats.NumInterpolatorScalars);

		if (bIncludeMessages)
		{
			const TArray<TSharedPtr<FJsonValue>> Messages = BuildMaterialCompileMessagesJson(Diagnostics, MaxMessages);
			Result->SetArrayField(TEXT("messages"), Messages);
			Result->SetNumberField(TEXT("message_count"), Messages.Num());
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleGetCompileResult(const FMCPRequest& Request)
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

	bool bCompile = false;
	Request.Params->TryGetBoolField(TEXT("compile"), bCompile);

	bool bIncludeMessages = true;
	Request.Params->TryGetBoolField(TEXT("include_messages"), bIncludeMessages);

	int32 MaxMessages = INDEX_NONE;
	double MaxMessagesNumber = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("max_messages"), MaxMessagesNumber))
	{
		if (MaxMessagesNumber < 0.0)
		{
			return InvalidParams(Request.Id, TEXT("'max_messages' must be >= 0"));
		}
		MaxMessages = FMath::FloorToInt(MaxMessagesNumber);
	}

	auto Task = [AssetPath, bCompile, bIncludeMessages, MaxMessages]() -> TSharedPtr<FJsonObject>
	{
		FString MaterialInterfacePath;
		FString Error;
		UMaterialInterface* MaterialInterface = ResolveMaterialInterfaceAsset(AssetPath, MaterialInterfacePath, Error);
		if (!MaterialInterface)
		{
			return MakeFailure(Error);
		}

		UMaterial* Material = Cast<UMaterial>(MaterialInterface);
		if (!Material)
		{
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface);
			Material = const_cast<UMaterial*>(Instance ? Instance->GetMaterial() : nullptr);
		}
		if (!Material)
		{
			return MakeFailure(TEXT("Unable to resolve a source material for diagnostics"));
		}

		if (bCompile)
		{
			UMaterialEditingLibrary::RecompileMaterial(Material);
		}

		FMaterialCompileDiagnostics Diagnostics;
		GatherMaterialCompileDiagnostics(Material, Diagnostics);

		TArray<TSharedPtr<FJsonValue>> ErrorNodesJson;
		for (const TWeakObjectPtr<UMaterialExpression>& ErrorExpression : Diagnostics.ErrorExpressions)
		{
			if (UMaterialExpression* Node = ErrorExpression.Get())
			{
				ErrorNodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
			}
		}

		TArray<TSharedPtr<FJsonValue>> CompileErrorsJson;
		for (const FString& CompileError : Diagnostics.CompileErrors)
		{
			CompileErrorsJson.Add(MakeShared<FJsonValueString>(CompileError));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), MaterialInterfacePath);
		Result->SetStringField(TEXT("material_path"), NormalizeAssetPath(Material->GetPathName()));
		Result->SetBoolField(TEXT("compiled"), bCompile);
		Result->SetBoolField(TEXT("is_compiling"), Diagnostics.bIsCompiling);
		Result->SetBoolField(TEXT("had_compile_error"), Diagnostics.bHadCompileError);
		Result->SetNumberField(TEXT("num_errors"), Diagnostics.CompileErrors.Num());
		Result->SetArrayField(TEXT("compile_errors"), CompileErrorsJson);
		Result->SetArrayField(TEXT("error_nodes"), ErrorNodesJson);
		Result->SetNumberField(TEXT("error_node_count"), ErrorNodesJson.Num());

		if (bIncludeMessages)
		{
			const TArray<TSharedPtr<FJsonValue>> Messages = BuildMaterialCompileMessagesJson(Diagnostics, MaxMessages);
			Result->SetArrayField(TEXT("messages"), Messages);
			Result->SetNumberField(TEXT("message_count"), Messages.Num());
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleValidateMaterial(const FMCPRequest& Request)
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

	bool bCompile = false;
	Request.Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bIncludeIssues = true;
	Request.Params->TryGetBoolField(TEXT("include_issues"), bIncludeIssues);

	auto Task = [AssetPath, bCompile, bIncludeIssues]() -> TSharedPtr<FJsonObject>
	{
		FString ResolvedAssetPath;
		FString AssetKind;
		FString Error;
		UObject* Asset = ResolveMaterialManagedAsset(AssetPath, ResolvedAssetPath, AssetKind, Error);
		if (!Asset)
		{
			return MakeFailure(Error);
		}

		const bool bPackageDirtyBefore = Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false;

		if (bCompile)
		{
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				UMaterialEditingLibrary::RecompileMaterial(Material);
			}
		}

		FDataValidationContext ValidationContext;
		const EDataValidationResult ValidationResult = Asset->IsDataValid(ValidationContext);
		const bool bPackageDirtyAfter = Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false;

		FMaterialCompileDiagnostics Diagnostics;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			GatherMaterialCompileDiagnostics(Material, Diagnostics);
		}

		TArray<TSharedPtr<FJsonValue>> ValidationIssuesJson;
		if (bIncludeIssues)
		{
			ValidationIssuesJson.Reserve(ValidationContext.GetIssues().Num());
			for (const FDataValidationContext::FIssue& Issue : ValidationContext.GetIssues())
			{
				ValidationIssuesJson.Add(MakeShared<FJsonValueObject>(BuildMaterialValidationIssueJson(Issue)));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
		Result->SetStringField(TEXT("asset_kind"), AssetKind);
		Result->SetBoolField(TEXT("compiled"), bCompile && Cast<UMaterial>(Asset) != nullptr);
		Result->SetBoolField(TEXT("include_issues"), bIncludeIssues);
		Result->SetNumberField(TEXT("validation_result"), static_cast<int32>(ValidationResult));
		Result->SetStringField(TEXT("validation_result_name"), MaterialDataValidationResultToString(ValidationResult));
		Result->SetNumberField(TEXT("validation_issue_count"), ValidationContext.GetIssues().Num());
		Result->SetNumberField(TEXT("validation_num_errors"), ValidationContext.GetNumErrors());
		Result->SetNumberField(TEXT("validation_num_warnings"), ValidationContext.GetNumWarnings());
		Result->SetBoolField(TEXT("package_dirty_before"), bPackageDirtyBefore);
		Result->SetBoolField(TEXT("package_dirty_after"), bPackageDirtyAfter);
		Result->SetBoolField(TEXT("mutated"), bPackageDirtyBefore != bPackageDirtyAfter);
		Result->SetBoolField(TEXT("has_compile_errors"), Diagnostics.CompileErrors.Num() > 0);
		Result->SetNumberField(TEXT("compile_error_count"), Diagnostics.CompileErrors.Num());
		Result->SetBoolField(
			TEXT("preflight_passed"),
			ValidationContext.GetNumErrors() == 0
			&& ValidationResult != EDataValidationResult::Invalid
			&& Diagnostics.CompileErrors.Num() == 0
		);

		if (bIncludeIssues)
		{
			Result->SetArrayField(TEXT("validation_issues"), ValidationIssuesJson);
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleGetMaterialStatus(const FMCPRequest& Request)
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
		FString ResolvedAssetPath;
		FString AssetKind;
		FString Error;
		UObject* Asset = ResolveMaterialManagedAsset(AssetPath, ResolvedAssetPath, AssetKind, Error);
		if (!Asset)
		{
			return MakeFailure(Error);
		}

		FDataValidationContext ValidationContext;
		const EDataValidationResult ValidationResult = Asset->IsDataValid(ValidationContext);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
		Result->SetStringField(TEXT("asset_kind"), AssetKind);
		Result->SetBoolField(TEXT("package_dirty"), Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false);
		Result->SetNumberField(TEXT("validation_result"), static_cast<int32>(ValidationResult));
		Result->SetStringField(TEXT("validation_result_name"), MaterialDataValidationResultToString(ValidationResult));
		Result->SetNumberField(TEXT("validation_num_errors"), ValidationContext.GetNumErrors());
		Result->SetNumberField(TEXT("validation_num_warnings"), ValidationContext.GetNumWarnings());

		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			FMaterialCompileDiagnostics Diagnostics;
			GatherMaterialCompileDiagnostics(Material, Diagnostics);

			Result->SetBoolField(TEXT("is_compiling"), Diagnostics.bIsCompiling);
			Result->SetBoolField(TEXT("had_compile_error"), Diagnostics.bHadCompileError);
			Result->SetNumberField(TEXT("compile_error_count"), Diagnostics.CompileErrors.Num());
			Result->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
			Result->SetNumberField(TEXT("comment_count"), Material->GetEditorComments().Num());

			WriteMaterialSettings(Material, Result);
			return Result;
		}

		if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
		{
			TArray<UMaterialExpressionFunctionInput*> FunctionInputs;
			TArray<UMaterialExpressionFunctionOutput*> FunctionOutputs;
			GatherMaterialFunctionIONodes(Function, FunctionInputs, FunctionOutputs);

			Result->SetNumberField(TEXT("expression_count"), Function->GetExpressions().Num());
			Result->SetNumberField(TEXT("comment_count"), Function->GetEditorComments().Num());
			Result->SetNumberField(TEXT("input_count"), FunctionInputs.Num());
			Result->SetNumberField(TEXT("output_count"), FunctionOutputs.Num());
			Result->SetStringField(TEXT("state_id"), Function->StateId.IsValid() ? Function->StateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
			return Result;
		}

		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset))
		{
			FStaticParameterSet StaticParameters = Instance->GetStaticParameters();
			Result->SetStringField(TEXT("parent_path"), Instance->Parent ? NormalizeAssetPath(Instance->Parent->GetPathName()) : FString());
			Result->SetNumberField(TEXT("scalar_override_count"), Instance->ScalarParameterValues.Num());
			Result->SetNumberField(TEXT("vector_override_count"), Instance->VectorParameterValues.Num());
			Result->SetNumberField(TEXT("texture_override_count"), Instance->TextureParameterValues.Num());
			Result->SetNumberField(TEXT("static_switch_override_count"), StaticParameters.StaticSwitchParameters.Num());
			Result->SetNumberField(TEXT("static_component_mask_override_count"), StaticParameters.EditorOnly.StaticComponentMaskParameters.Num());
			return Result;
		}

		if (UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Asset))
		{
			Result->SetStringField(TEXT("state_id"), Collection->StateId.IsValid() ? Collection->StateId.ToString(EGuidFormats::DigitsWithHyphens) : FString());
			Result->SetNumberField(TEXT("scalar_parameter_count"), Collection->ScalarParameters.Num());
			Result->SetNumberField(TEXT("vector_parameter_count"), Collection->VectorParameters.Num());
			Result->SetBoolField(TEXT("has_base_collection"), Collection->GetBaseParameterCollection() != nullptr);
			Result->SetStringField(TEXT("base_collection_path"), Collection->GetBaseParameterCollection() ? NormalizeAssetPath(Collection->GetBaseParameterCollection()->GetPathName()) : FString());
			return Result;
		}

		return MakeFailure(TEXT("Unsupported material asset type"));
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleListMaterialWarnings(const FMCPRequest& Request)
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

	bool bCompile = false;
	Request.Params->TryGetBoolField(TEXT("compile"), bCompile);

	int32 MaxResults = INDEX_NONE;
	double MaxResultsNumber = 0.0;
	if (Request.Params->TryGetNumberField(TEXT("max_results"), MaxResultsNumber))
	{
		if (MaxResultsNumber < 0.0)
		{
			return InvalidParams(Request.Id, TEXT("'max_results' must be >= 0"));
		}
		MaxResults = FMath::FloorToInt(MaxResultsNumber);
	}

	auto Task = [AssetPath, bCompile, MaxResults]() -> TSharedPtr<FJsonObject>
	{
		FString ResolvedAssetPath;
		FString AssetKind;
		FString Error;
		UObject* Asset = ResolveMaterialManagedAsset(AssetPath, ResolvedAssetPath, AssetKind, Error);
		if (!Asset)
		{
			return MakeFailure(Error);
		}

		if (bCompile)
		{
			if (UMaterial* Material = Cast<UMaterial>(Asset))
			{
				UMaterialEditingLibrary::RecompileMaterial(Material);
			}
		}

		FDataValidationContext ValidationContext;
		const EDataValidationResult ValidationResult = Asset->IsDataValid(ValidationContext);

		int32 ValidationWarningCount = 0;
		int32 ValidationErrorCount = 0;
		TArray<TSharedPtr<FJsonValue>> WarningsJson;
		for (const FDataValidationContext::FIssue& Issue : ValidationContext.GetIssues())
		{
			const bool bIsWarning = Issue.Severity == EMessageSeverity::Warning || Issue.Severity == EMessageSeverity::PerformanceWarning;
			const bool bIsError = Issue.Severity == EMessageSeverity::Error;

			if (bIsWarning)
			{
				++ValidationWarningCount;
				if (MaxResults < 0 || WarningsJson.Num() < MaxResults)
				{
					WarningsJson.Add(MakeShared<FJsonValueObject>(BuildMaterialValidationIssueJson(Issue)));
				}
			}
			else if (bIsError)
			{
				++ValidationErrorCount;
			}
		}

		FMaterialCompileDiagnostics Diagnostics;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			GatherMaterialCompileDiagnostics(Material, Diagnostics);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
		Result->SetStringField(TEXT("asset_kind"), AssetKind);
		Result->SetBoolField(TEXT("compiled"), bCompile && Cast<UMaterial>(Asset) != nullptr);
		Result->SetNumberField(TEXT("validation_result"), static_cast<int32>(ValidationResult));
		Result->SetStringField(TEXT("validation_result_name"), MaterialDataValidationResultToString(ValidationResult));
		Result->SetNumberField(TEXT("compile_error_count"), Diagnostics.CompileErrors.Num());
		Result->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
		Result->SetNumberField(TEXT("validation_warning_count"), ValidationWarningCount);
		Result->SetNumberField(TEXT("warning_count"), ValidationWarningCount);
		Result->SetNumberField(TEXT("returned_warning_count"), WarningsJson.Num());
		Result->SetArrayField(TEXT("warnings"), WarningsJson);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleGetShaderStats(const FMCPRequest& Request)
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
		FString MaterialInterfacePath;
		FString Error;
		UMaterialInterface* MaterialInterface = ResolveMaterialInterfaceAsset(AssetPath, MaterialInterfacePath, Error);
		if (!MaterialInterface)
		{
			return MakeFailure(Error);
		}

		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(MaterialInterface);
		const double EstimatedCost = static_cast<double>(Stats.NumPixelShaderInstructions)
			+ static_cast<double>(Stats.NumVertexShaderInstructions) * 0.25
			+ static_cast<double>(Stats.NumSamplers) * 4.0
			+ static_cast<double>(Stats.NumVirtualTextureSamples) * 2.0;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), MaterialInterfacePath);
		Result->SetStringField(TEXT("asset_kind"), MaterialInterface->IsA<UMaterial>() ? TEXT("material") : TEXT("material_instance"));
		Result->SetStringField(TEXT("material_path"), NormalizeAssetPath(MaterialInterface->GetPathName()));
		if (const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(MaterialInterface))
		{
			Result->SetStringField(TEXT("parent_path"), MaterialInstance->Parent ? NormalizeAssetPath(MaterialInstance->Parent->GetPathName()) : FString());
		}
		Result->SetNumberField(TEXT("num_vertex_shader_instructions"), Stats.NumVertexShaderInstructions);
		Result->SetNumberField(TEXT("num_pixel_shader_instructions"), Stats.NumPixelShaderInstructions);
		Result->SetNumberField(TEXT("num_samplers"), Stats.NumSamplers);
		Result->SetNumberField(TEXT("num_vertex_texture_samples"), Stats.NumVertexTextureSamples);
		Result->SetNumberField(TEXT("num_pixel_texture_samples"), Stats.NumPixelTextureSamples);
		Result->SetNumberField(TEXT("num_virtual_texture_samples"), Stats.NumVirtualTextureSamples);
		Result->SetNumberField(TEXT("num_uv_scalars"), Stats.NumUVScalars);
		Result->SetNumberField(TEXT("num_interpolator_scalars"), Stats.NumInterpolatorScalars);
		Result->SetNumberField(TEXT("estimated_cost"), EstimatedCost);
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleBeginTransaction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString AssetPath;
	FString Description = TEXT("SpecialAgent Material Transaction");
	FString TransactionContext = TEXT("SpecialAgent.Material");
	if (!Request.Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'asset_path'"));
	}
	Request.Params->TryGetStringField(TEXT("description"), Description);
	Request.Params->TryGetStringField(TEXT("transaction_context"), TransactionContext);

	auto Task = [this, AssetPath, Description, TransactionContext]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		if (!GEditor)
		{
			return Fail(TEXT("Editor is unavailable"));
		}

		if (ActiveTransaction.IsSet() && !GEditor->IsTransactionActive())
		{
			ActiveTransaction.Reset();
		}

		if (ActiveTransaction.IsSet())
		{
			return Fail(FString::Printf(
				TEXT("A managed material transaction is already active (%s). End or cancel it first."),
				*ActiveTransaction->TransactionId
			));
		}
		if (GEditor->IsTransactionActive())
		{
			return Fail(FString::Printf(
				TEXT("Another editor transaction is already active ('%s'). Begin transaction aborted for safety."),
				*GEditor->GetTransactionName().ToString()
			));
		}

		FString ResolvedAssetPath;
		FString AssetKind;
		FString Error;
		UObject* Asset = ResolveMaterialManagedAsset(AssetPath, ResolvedAssetPath, AssetKind, Error);
		if (!Asset)
		{
			return Fail(Error);
		}

		const int32 TransactionIndex = GEditor->BeginTransaction(*TransactionContext, FText::FromString(Description), Asset);
		if (TransactionIndex == INDEX_NONE)
		{
			return Fail(TEXT("Failed to begin transaction"));
		}

		Asset->Modify();

		++TransactionSequence;
		FMaterialTransactionState TransactionState;
		TransactionState.TransactionId = FString::Printf(
			TEXT("mat_tx_%d_%s"),
			TransactionSequence,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)
		);
		TransactionState.AssetPath = ResolvedAssetPath;
		TransactionState.TransactionIndex = TransactionIndex;
		TransactionState.Description = Description;
		TransactionState.StartedAtUtc = FDateTime::UtcNow();

		ActiveTransaction = TransactionState;

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("transaction_id"), TransactionState.TransactionId);
		Result->SetStringField(TEXT("asset_path"), TransactionState.AssetPath);
		Result->SetNumberField(TEXT("transaction_index"), TransactionState.TransactionIndex);
		Result->SetStringField(TEXT("description"), TransactionState.Description);
		Result->SetStringField(TEXT("transaction_context"), TransactionContext);
		Result->SetStringField(TEXT("started_at_utc"), TransactionState.StartedAtUtc.ToIso8601());
		Result->SetBoolField(TEXT("is_transaction_active"), GEditor->IsTransactionActive());
		Result->SetStringField(TEXT("active_transaction_name"), GEditor->GetTransactionName().ToString());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleEndTransaction(const FMCPRequest& Request)
{
	FString RequestedTransactionId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("transaction_id"), RequestedTransactionId);
	}

	auto Task = [this, RequestedTransactionId]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		if (!GEditor)
		{
			return Fail(TEXT("Editor is unavailable"));
		}

		if (!ActiveTransaction.IsSet())
		{
			if (GEditor->IsTransactionActive())
			{
				return Fail(FString::Printf(
					TEXT("An external transaction is active ('%s'). Refusing to end unknown transaction."),
					*GEditor->GetTransactionName().ToString()
				));
			}
			return Fail(TEXT("No managed material transaction is active"));
		}

		if (!RequestedTransactionId.IsEmpty() && !RequestedTransactionId.Equals(ActiveTransaction->TransactionId, ESearchCase::CaseSensitive))
		{
			return Fail(FString::Printf(TEXT("transaction_id mismatch. Expected '%s'"), *ActiveTransaction->TransactionId));
		}

		const FMaterialTransactionState CompletedTransaction = ActiveTransaction.GetValue();
		if (!GEditor->IsTransactionActive())
		{
			ActiveTransaction.Reset();
			return Fail(TEXT("Managed transaction became inactive before end_transaction was called"));
		}

		const int32 EndedTransactionIndex = GEditor->EndTransaction();
		ActiveTransaction.Reset();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("transaction_id"), CompletedTransaction.TransactionId);
		Result->SetStringField(TEXT("asset_path"), CompletedTransaction.AssetPath);
		Result->SetNumberField(TEXT("transaction_index"), CompletedTransaction.TransactionIndex);
		Result->SetNumberField(TEXT("ended_transaction_index"), EndedTransactionIndex);
		Result->SetStringField(TEXT("description"), CompletedTransaction.Description);
		Result->SetStringField(TEXT("started_at_utc"), CompletedTransaction.StartedAtUtc.ToIso8601());
		Result->SetBoolField(TEXT("is_transaction_active"), GEditor->IsTransactionActive());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCancelTransaction(const FMCPRequest& Request)
{
	FString RequestedTransactionId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("transaction_id"), RequestedTransactionId);
	}

	auto Task = [this, RequestedTransactionId]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		if (!GEditor)
		{
			return Fail(TEXT("Editor is unavailable"));
		}

		if (!ActiveTransaction.IsSet())
		{
			if (GEditor->IsTransactionActive())
			{
				return Fail(FString::Printf(
					TEXT("An external transaction is active ('%s'). Refusing to cancel unknown transaction."),
					*GEditor->GetTransactionName().ToString()
				));
			}
			return Fail(TEXT("No managed material transaction is active"));
		}

		if (!RequestedTransactionId.IsEmpty() && !RequestedTransactionId.Equals(ActiveTransaction->TransactionId, ESearchCase::CaseSensitive))
		{
			return Fail(FString::Printf(TEXT("transaction_id mismatch. Expected '%s'"), *ActiveTransaction->TransactionId));
		}

		const FMaterialTransactionState CancelledTransaction = ActiveTransaction.GetValue();
		if (!GEditor->IsTransactionActive())
		{
			ActiveTransaction.Reset();
			return Fail(TEXT("Managed transaction became inactive before cancel_transaction was called"));
		}

		const int32 EndedTransactionIndex = GEditor->EndTransaction();
		const bool bCanUndo = GEditor->Trans && GEditor->Trans->CanUndo();
		const bool bRolledBack = bCanUndo ? GEditor->Trans->Undo() : false;
		ActiveTransaction.Reset();

		if (!bRolledBack)
		{
			return Fail(TEXT("Transaction was ended but rollback failed. Manual undo may be required."));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("transaction_id"), CancelledTransaction.TransactionId);
		Result->SetStringField(TEXT("asset_path"), CancelledTransaction.AssetPath);
		Result->SetNumberField(TEXT("transaction_index"), CancelledTransaction.TransactionIndex);
		Result->SetNumberField(TEXT("ended_transaction_index"), EndedTransactionIndex);
		Result->SetStringField(TEXT("description"), CancelledTransaction.Description);
		Result->SetStringField(TEXT("started_at_utc"), CancelledTransaction.StartedAtUtc.ToIso8601());
		Result->SetBoolField(TEXT("cancelled"), true);
		Result->SetBoolField(TEXT("rolled_back"), true);
		Result->SetBoolField(TEXT("is_transaction_active"), GEditor->IsTransactionActive());
		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleDryRunValidate(const FMCPRequest& Request)
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

	bool bIncludeIssues = true;
	Request.Params->TryGetBoolField(TEXT("include_issues"), bIncludeIssues);

	auto Task = [AssetPath, bIncludeIssues]() -> TSharedPtr<FJsonObject>
	{
		FString ResolvedAssetPath;
		FString AssetKind;
		FString Error;
		UObject* Asset = ResolveMaterialManagedAsset(AssetPath, ResolvedAssetPath, AssetKind, Error);
		if (!Asset)
		{
			return MakeFailure(Error);
		}

		const bool bPackageDirtyBefore = Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false;
		FDataValidationContext ValidationContext;
		const EDataValidationResult ValidationResult = Asset->IsDataValid(ValidationContext);
		const bool bPackageDirtyAfter = Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false;

		FMaterialCompileDiagnostics Diagnostics;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			GatherMaterialCompileDiagnostics(Material, Diagnostics);
		}

		TArray<TSharedPtr<FJsonValue>> ValidationIssuesJson;
		if (bIncludeIssues)
		{
			ValidationIssuesJson.Reserve(ValidationContext.GetIssues().Num());
			for (const FDataValidationContext::FIssue& Issue : ValidationContext.GetIssues())
			{
				ValidationIssuesJson.Add(MakeShared<FJsonValueObject>(BuildMaterialValidationIssueJson(Issue)));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
		Result->SetStringField(TEXT("asset_kind"), AssetKind);
		Result->SetBoolField(TEXT("include_issues"), bIncludeIssues);
		Result->SetBoolField(TEXT("mutated"), bPackageDirtyBefore != bPackageDirtyAfter);
		Result->SetBoolField(TEXT("package_dirty_before"), bPackageDirtyBefore);
		Result->SetBoolField(TEXT("package_dirty_after"), bPackageDirtyAfter);
		Result->SetNumberField(TEXT("validation_result"), static_cast<int32>(ValidationResult));
		Result->SetStringField(TEXT("validation_result_name"), MaterialDataValidationResultToString(ValidationResult));
		Result->SetNumberField(TEXT("validation_issue_count"), ValidationContext.GetIssues().Num());
		Result->SetNumberField(TEXT("validation_num_errors"), ValidationContext.GetNumErrors());
		Result->SetNumberField(TEXT("validation_num_warnings"), ValidationContext.GetNumWarnings());
		Result->SetBoolField(TEXT("has_compile_errors"), Diagnostics.CompileErrors.Num() > 0);
		Result->SetNumberField(TEXT("compile_error_count"), Diagnostics.CompileErrors.Num());
		Result->SetBoolField(
			TEXT("preflight_passed"),
			ValidationContext.GetNumErrors() == 0
			&& ValidationResult != EDataValidationResult::Invalid
			&& Diagnostics.CompileErrors.Num() == 0
		);

		if (bIncludeIssues)
		{
			Result->SetArrayField(TEXT("validation_issues"), ValidationIssuesJson);
		}

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}

FMCPResponse FMaterialService::HandleCapabilities(const FMCPRequest& Request)
{
	auto Task = [this]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		if (ActiveTransaction.IsSet() && (!GEditor || !GEditor->IsTransactionActive()))
		{
			ActiveTransaction.Reset();
		}

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
		PhasesObj->SetBoolField(TEXT("phase_5_parameter_authoring"), true);
		PhasesObj->SetBoolField(TEXT("phase_6_material_instance_support"), true);
		PhasesObj->SetBoolField(TEXT("phase_7_material_function_support"), true);
		PhasesObj->SetBoolField(TEXT("phase_8_parameter_collection_support"), true);
		PhasesObj->SetBoolField(TEXT("phase_9_refactor_symbol_operations"), true);
		PhasesObj->SetBoolField(TEXT("phase_10_compile_diagnostics_validation"), true);
		PhasesObj->SetBoolField(TEXT("phase_11_transactions_and_safety"), true);
		PhasesObj->SetBoolField(TEXT("phase_12_material_type_coverage"), true);
		Result->SetObjectField(TEXT("phases"), PhasesObj);

		TSharedPtr<FJsonObject> Phase11Tools = MakeShared<FJsonObject>();
		Phase11Tools->SetBoolField(TEXT("begin_transaction"), true);
		Phase11Tools->SetBoolField(TEXT("end_transaction"), true);
		Phase11Tools->SetBoolField(TEXT("cancel_transaction"), true);
		Phase11Tools->SetBoolField(TEXT("dry_run_validate"), true);
		Phase11Tools->SetBoolField(TEXT("capabilities"), true);
		Result->SetObjectField(TEXT("phase11_tools"), Phase11Tools);

		TSharedPtr<FJsonObject> TypeCoverage = MakeShared<FJsonObject>();
		TypeCoverage->SetBoolField(TEXT("surface"), true);
		TypeCoverage->SetBoolField(TEXT("deferred_decal"), true);
		TypeCoverage->SetBoolField(TEXT("light_function"), true);
		TypeCoverage->SetBoolField(TEXT("post_process"), true);
		TypeCoverage->SetBoolField(TEXT("ui"), true);
		TypeCoverage->SetBoolField(TEXT("volume"), true);
		TypeCoverage->SetBoolField(TEXT("material_layer_blend_minimum_support"), true);
		TypeCoverage->SetBoolField(TEXT("association_layer_parameters"), true);
		TypeCoverage->SetBoolField(TEXT("association_blend_parameters"), true);
		Result->SetObjectField(TEXT("phase12_type_coverage"), TypeCoverage);

		TSharedPtr<FJsonObject> DependenciesObj = MakeShared<FJsonObject>();
		DependenciesObj->SetBoolField(TEXT("material_editor_module_exists"), FModuleManager::Get().ModuleExists(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("material_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("asset_registry_module_exists"), FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")));
		DependenciesObj->SetBoolField(TEXT("asset_tools_module_exists"), FModuleManager::Get().ModuleExists(TEXT("AssetTools")));
		DependenciesObj->SetBoolField(TEXT("asset_tools_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")));
		DependenciesObj->SetBoolField(TEXT("unreal_ed_module_exists"), FModuleManager::Get().ModuleExists(TEXT("UnrealEd")));
		DependenciesObj->SetBoolField(TEXT("editor_scripting_utilities_module_exists"), FModuleManager::Get().ModuleExists(TEXT("EditorScriptingUtilities")));
		Result->SetObjectField(TEXT("dependencies"), DependenciesObj);

		TSharedPtr<FJsonObject> RuntimeObj = MakeShared<FJsonObject>();
		RuntimeObj->SetBoolField(TEXT("editor_available"), GEditor != nullptr);
		RuntimeObj->SetBoolField(TEXT("editor_transaction_active"), GEditor ? GEditor->IsTransactionActive() : false);
		RuntimeObj->SetStringField(TEXT("editor_transaction_name"), GEditor ? GEditor->GetTransactionName().ToString() : FString());
		RuntimeObj->SetBoolField(TEXT("managed_transaction_active"), ActiveTransaction.IsSet());
		if (ActiveTransaction.IsSet())
		{
			TSharedPtr<FJsonObject> ActiveObj = MakeShared<FJsonObject>();
			ActiveObj->SetStringField(TEXT("transaction_id"), ActiveTransaction->TransactionId);
			ActiveObj->SetStringField(TEXT("asset_path"), ActiveTransaction->AssetPath);
			ActiveObj->SetNumberField(TEXT("transaction_index"), ActiveTransaction->TransactionIndex);
			ActiveObj->SetStringField(TEXT("description"), ActiveTransaction->Description);
			ActiveObj->SetStringField(TEXT("started_at_utc"), ActiveTransaction->StartedAtUtc.ToIso8601());
			RuntimeObj->SetObjectField(TEXT("active_transaction"), ActiveObj);
		}
		Result->SetObjectField(TEXT("runtime_state"), RuntimeObj);

		TArray<TSharedPtr<FJsonValue>> NotesJson;
		NotesJson.Add(MakeShared<FJsonValueString>(TEXT("Only one managed material transaction can be active at a time.")));
		NotesJson.Add(MakeShared<FJsonValueString>(TEXT("Managed transaction tools refuse to end/cancel unknown external transactions.")));
		NotesJson.Add(MakeShared<FJsonValueString>(TEXT("cancel_transaction performs rollback by ending the transaction and issuing a single Undo.")));
		NotesJson.Add(MakeShared<FJsonValueString>(TEXT("dry_run_validate performs non-mutating validation checks and does not compile.")));
		Result->SetArrayField(TEXT("notes"), NotesJson);

		return Result;
	};

	return FMCPResponse::Success(Request.Id, FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task));
}
