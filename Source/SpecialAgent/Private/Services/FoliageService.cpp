// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/FoliageService.h"
#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"

FFoliageService::FFoliageService()
{
}

FString FFoliageService::GetServiceDescription() const
{
	return TEXT("Foliage management - paint and remove instanced foliage");
}

// Helper function to execute Python code from request params
FMCPResponse FFoliageService::ExecutePythonFromParams(const FMCPRequest& Request)
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

FMCPResponse FFoliageService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("paint_in_area")) return HandlePaintInArea(Request);
	if (MethodName == TEXT("remove_from_area")) return HandleRemoveFromArea(Request);
	if (MethodName == TEXT("get_density")) return HandleGetDensity(Request);

	return MethodNotFound(Request.Id, TEXT("foliage"), MethodName);
}

FMCPResponse FFoliageService::HandlePaintInArea(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FFoliageService::HandleRemoveFromArea(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FFoliageService::HandleGetDensity(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}


TArray<FMCPToolInfo> FFoliageService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	// TODO: Add tool definitions
	return Tools;
}
