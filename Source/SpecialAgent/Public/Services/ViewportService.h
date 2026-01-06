// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Viewport Service
 * 
 * Control editor viewport camera for optimal screenshot capture.
 * Methods: set_location, set_rotation, get_transform, focus_actor
 */
class SPECIALAGENT_API FViewportService : public IMCPService
{
public:
	FViewportService();
	virtual ~FViewportService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSetLocation(const FMCPRequest& Request);
	FMCPResponse HandleSetRotation(const FMCPRequest& Request);
	FMCPResponse HandleGetTransform(const FMCPRequest& Request);
	FMCPResponse HandleFocusActor(const FMCPRequest& Request);
	FMCPResponse HandleTraceFromScreen(const FMCPRequest& Request);
};

