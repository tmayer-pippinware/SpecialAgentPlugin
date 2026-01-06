// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/LightingService.h"
#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"

FLightingService::FLightingService()
{
}

FString FLightingService::GetServiceDescription() const
{
	return TEXT("Lighting control - spawn lights, configure, and build lightmaps");
}

// Helper function to execute Python code from request params
FMCPResponse FLightingService::ExecutePythonFromParams(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid() || !Request.Params->HasField(TEXT("code")))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter: 'code' (Python script)"));
	}

	FString Code = Request.Params->GetStringField(TEXT("code"));
	float Timeout = Request.Params->HasField(TEXT("timeout")) ? Request.Params->GetNumberField(TEXT("timeout")) : 30.0f;

	FPythonService PythonService;
	TSharedPtr<FJsonObject> PythonParams = MakeShared<FJsonObject>();
	PythonParams->SetStringField(TEXT("code"), Code);
	PythonParams->SetNumberField(TEXT("timeout"), Timeout);
	
	FMCPRequest PythonRequest;
	PythonRequest.JsonRpc = Request.JsonRpc;
	PythonRequest.Id = Request.Id;
	PythonRequest.Method = TEXT("python/execute");
	PythonRequest.Params = PythonParams;
	
	return PythonService.HandleExecute(PythonRequest);
}

FMCPResponse FLightingService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("spawn_light")) return HandleSpawnLight(Request);
	if (MethodName == TEXT("set_light_intensity")) return HandleSetLightIntensity(Request);
	if (MethodName == TEXT("set_light_color")) return HandleSetLightColor(Request);
	if (MethodName == TEXT("build_lighting")) return HandleBuildLighting(Request);

	return MethodNotFound(Request.Id, TEXT("lighting"), MethodName);
}

FMCPResponse FLightingService::HandleSpawnLight(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLightingService::HandleSetLightIntensity(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLightingService::HandleSetLightColor(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLightingService::HandleBuildLighting(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}



TArray<FMCPToolInfo> FLightingService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	// TODO: Add tool definitions
	return Tools;
}
