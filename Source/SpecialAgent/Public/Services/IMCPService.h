// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCPServer.h"

/**
 * MCP Tool Information
 * Describes a single tool (method) that can be called
 */
struct FMCPToolInfo
{
	FString Name;                                  // Method name (without service prefix)
	FString Description;                           // Human-readable description
	TSharedPtr<FJsonObject> Parameters;            // JSON schema for parameters
	TArray<FString> RequiredParams;                // List of required parameter names
	
	FMCPToolInfo()
		: Parameters(MakeShared<FJsonObject>())
	{}
};

/**
 * MCP Service Interface
 * 
 * Base interface for all MCP service implementations.
 * Each service handles a specific domain of functionality (assets, world, python, etc.)
 */
class SPECIALAGENT_API IMCPService
{
public:
	virtual ~IMCPService() = default;

	/**
	 * Handle an MCP request for this service
	 * @param Request The MCP request
	 * @param MethodName The method name (without service prefix)
	 * @return The MCP response
	 */
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) = 0;

	/**
	 * Get a description of this service
	 * @return Service description
	 */
	virtual FString GetServiceDescription() const = 0;
	
	/**
	 * Get list of available tools provided by this service
	 * @return Array of tool information
	 */
	virtual TArray<FMCPToolInfo> GetAvailableTools() const = 0;

protected:
	/**
	 * Helper function to create a method not found error
	 */
	FMCPResponse MethodNotFound(const FString& RequestId, const FString& ServiceName, const FString& MethodName) const
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("service"), ServiceName);
		ErrorData->SetStringField(TEXT("method"), MethodName);

		return FMCPResponse::Error(
			RequestId,
			-32601,
			FString::Printf(TEXT("Method not found: %s/%s"), *ServiceName, *MethodName),
			ErrorData
		);
	}

	/**
	 * Helper function to create an invalid params error
	 */
	FMCPResponse InvalidParams(const FString& RequestId, const FString& Reason) const
	{
		return FMCPResponse::Error(
			RequestId,
			-32602,
			FString::Printf(TEXT("Invalid params: %s"), *Reason)
		);
	}

	/**
	 * Helper function to create an internal error
	 */
	FMCPResponse InternalError(const FString& RequestId, const FString& ErrorMessage) const
	{
		return FMCPResponse::Error(
			RequestId,
			-32603,
			FString::Printf(TEXT("Internal error: %s"), *ErrorMessage)
		);
	}
};

