// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Material Service
 *
 * Baseline scaffold for native material authoring tools.
 */
class SPECIALAGENT_API FMaterialService : public IMCPService
{
public:
	FMaterialService();
	virtual ~FMaterialService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleCapabilities(const FMCPRequest& Request);
};
