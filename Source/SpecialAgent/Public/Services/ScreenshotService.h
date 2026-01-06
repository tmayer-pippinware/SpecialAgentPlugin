// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Screenshot Service
 * 
 * CRITICAL visual feedback loop - capture viewport for LLM vision analysis.
 * Methods: capture, save
 */
class SPECIALAGENT_API FScreenshotService : public IMCPService
{
public:
	FScreenshotService();
	virtual ~FScreenshotService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleCapture(const FMCPRequest& Request);
	FMCPResponse HandleSave(const FMCPRequest& Request);
};

