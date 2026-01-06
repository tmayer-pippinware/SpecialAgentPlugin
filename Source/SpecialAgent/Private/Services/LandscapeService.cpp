// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/LandscapeService.h"
#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"

FLandscapeService::FLandscapeService()
{
}

FString FLandscapeService::GetServiceDescription() const
{
	return TEXT("Landscape terrain editing - sculpt, flatten, smooth, and paint layers");
}

// Helper function to execute Python code from request params
FMCPResponse FLandscapeService::ExecutePythonFromParams(const FMCPRequest& Request)
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

FMCPResponse FLandscapeService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("get_info")) return HandleGetInfo(Request);
	if (MethodName == TEXT("sculpt_height")) return HandleSculptHeight(Request);
	if (MethodName == TEXT("flatten_area")) return HandleFlattenArea(Request);
	if (MethodName == TEXT("smooth_area")) return HandleSmoothArea(Request);
	if (MethodName == TEXT("paint_layer")) return HandlePaintLayer(Request);

	return MethodNotFound(Request.Id, TEXT("landscape"), MethodName);
}

FMCPResponse FLandscapeService::HandleGetInfo(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLandscapeService::HandleSculptHeight(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLandscapeService::HandleFlattenArea(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLandscapeService::HandleSmoothArea(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FLandscapeService::HandlePaintLayer(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}


TArray<FMCPToolInfo> FLandscapeService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	// TODO: Add tool definitions
	return Tools;
}
