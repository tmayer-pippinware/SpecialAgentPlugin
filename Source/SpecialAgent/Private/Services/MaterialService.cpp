// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/MaterialService.h"

#include "GameThreadDispatcher.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"

FMaterialService::FMaterialService()
{
}

FString FMaterialService::GetServiceDescription() const
{
	return TEXT("Material authoring - baseline scaffold and capability reporting");
}

TArray<FMCPToolInfo> FMaterialService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("capabilities");
		Tool.Description = TEXT("Report baseline material service capabilities and module availability.");
		Tools.Add(Tool);
	}

	return Tools;
}

FMCPResponse FMaterialService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("capabilities"))
	{
		return HandleCapabilities(Request);
	}

	return MethodNotFound(Request.Id, TEXT("material"), MethodName);
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

		TSharedPtr<FJsonObject> DependenciesObj = MakeShared<FJsonObject>();
		DependenciesObj->SetBoolField(TEXT("material_editor_module_exists"), FModuleManager::Get().ModuleExists(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("material_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("MaterialEditor")));
		DependenciesObj->SetBoolField(TEXT("asset_registry_module_exists"), FModuleManager::Get().ModuleExists(TEXT("AssetRegistry")));
		DependenciesObj->SetBoolField(TEXT("unreal_ed_module_exists"), FModuleManager::Get().ModuleExists(TEXT("UnrealEd")));
		DependenciesObj->SetBoolField(TEXT("editor_scripting_utilities_module_exists"), FModuleManager::Get().ModuleExists(TEXT("EditorScriptingUtilities")));
		Result->SetObjectField(TEXT("dependencies"), DependenciesObj);

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}
