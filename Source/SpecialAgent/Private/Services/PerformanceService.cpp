// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/PerformanceService.h"
#include "Services/PythonService.h"
#include "GameThreadDispatcher.h"

FPerformanceService::FPerformanceService()
{
}

FString FPerformanceService::GetServiceDescription() const
{
	return TEXT("Performance analysis - level statistics, bounds checking, and overlap detection");
}

// Helper function to execute Python code from request params
FMCPResponse FPerformanceService::ExecutePythonFromParams(const FMCPRequest& Request)
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

FMCPResponse FPerformanceService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("get_statistics")) return HandleGetStatistics(Request);
	if (MethodName == TEXT("get_actor_bounds")) return HandleGetActorBounds(Request);
	if (MethodName == TEXT("check_overlaps")) return HandleCheckOverlaps(Request);

	return MethodNotFound(Request.Id, TEXT("performance"), MethodName);
}

FMCPResponse FPerformanceService::HandleGetStatistics(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FPerformanceService::HandleGetActorBounds(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}

FMCPResponse FPerformanceService::HandleCheckOverlaps(const FMCPRequest& Request)
{
	return ExecutePythonFromParams(Request);
}


TArray<FMCPToolInfo> FPerformanceService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	// TODO: Add tool definitions
	return Tools;
}
