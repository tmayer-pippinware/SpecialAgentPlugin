// Copyright Epic Games, Inc. All Rights Reserved.

#include "MCPStatusBarWidget.h"
#include "MCPServer.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "HAL/PlatformApplicationMisc.h"

#define LOCTEXT_NAMESPACE "MCPStatusBarWidget"

void SMCPStatusBarWidget::Construct(const FArguments& InArgs, TSharedPtr<FSpecialAgentMCPServer> InMCPServer)
{
	MCPServer = InMCPServer;
	CachedStatus = EMCPServerStatus::Offline;
	ConnectedClients = 0;

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked(this, &SMCPStatusBarWidget::OnStatusClicked)
		.ToolTipText(this, &SMCPStatusBarWidget::GetStatusTooltip)
		.ContentPadding(FMargin(4.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
				.ColorAndOpacity(this, &SMCPStatusBarWidget::GetStatusColor)
				.DesiredSizeOverride(FVector2D(10, 10))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MCPLabel", "SpecialAgent"))
				.TextStyle(FAppStyle::Get(), "SmallText")
			]
		]
	];

	// Register timer to update status every 0.5 seconds
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SMCPStatusBarWidget::UpdateStatus));
}

FSlateColor SMCPStatusBarWidget::GetStatusColor() const
{
	switch (CachedStatus)
	{
		case EMCPServerStatus::Connected:
			return FSlateColor(FLinearColor::Green);
		case EMCPServerStatus::Listening:
			return FSlateColor(FLinearColor(1.0f, 0.5f, 0.0f)); // Orange
		case EMCPServerStatus::Offline:
		default:
			return FSlateColor(FLinearColor::Red);
	}
}

FText SMCPStatusBarWidget::GetStatusTooltip() const
{
	switch (CachedStatus)
	{
		case EMCPServerStatus::Connected:
			return FText::Format(
				LOCTEXT("MCPConnectedTooltip", "MCP Server: Connected ({0} client(s))\nPort: 8767\nClick to copy config to clipboard"),
				FText::AsNumber(ConnectedClients)
			);
		case EMCPServerStatus::Listening:
			return LOCTEXT("MCPListeningTooltip", "MCP Server: Listening\nPort: 8767\nWaiting for MCP client...\nClick to copy config to clipboard");
		case EMCPServerStatus::Offline:
		default:
			return LOCTEXT("MCPOfflineTooltip", "MCP Server: Offline\nServer failed to start or is disabled.\nClick to attempt restart");
	}
}

EMCPServerStatus SMCPStatusBarWidget::GetServerStatus() const
{
	TSharedPtr<FSpecialAgentMCPServer> Server = MCPServer.Pin();
	if (!Server.IsValid() || !Server->IsRunning())
	{
		return EMCPServerStatus::Offline;
	}

	// For now, we'll show Listening when running
	// In the future, we could track actual client connections
	int32 ClientCount = Server->GetConnectedClientCount();
	if (ClientCount > 0)
	{
		return EMCPServerStatus::Connected;
	}

	return EMCPServerStatus::Listening;
}

FReply SMCPStatusBarWidget::OnStatusClicked()
{
	TSharedPtr<FSpecialAgentMCPServer> Server = MCPServer.Pin();
	
	// MCP configuration JSON - use /mcp endpoint for streamable HTTP transport
	const FString ConfigJson = TEXT("{\n  \"mcpServers\": {\n    \"SpecialAgent\": {\n      \"url\": \"http://localhost:8767/mcp\"\n    }\n  }\n}");
	
	FText Message;
	SNotificationItem::ECompletionState State;

	switch (CachedStatus)
	{
		case EMCPServerStatus::Connected:
			// Copy config to clipboard
			FPlatformApplicationMisc::ClipboardCopy(*ConfigJson);
			Message = FText::Format(
				LOCTEXT("MCPConnectedMessage", "MCP Server Connected ({0} client(s))\n\nConfiguration copied to clipboard!\n\nEndpoints:\n• SSE: http://localhost:8767/sse\n• Message: http://localhost:8767/message\n• Health: http://localhost:8767/health"),
				FText::AsNumber(ConnectedClients)
			);
			State = SNotificationItem::CS_Success;
			break;

		case EMCPServerStatus::Listening:
			// Copy config to clipboard
			FPlatformApplicationMisc::ClipboardCopy(*ConfigJson);
			Message = LOCTEXT("MCPListeningMessage", "MCP Server Listening - Configuration copied to clipboard!\n\nPaste this into your MCP client config:\n{\n  \"mcpServers\": {\n    \"SpecialAgent\": {\n      \"url\": \"http://localhost:8767/mcp\"\n    }\n  }\n}");
			State = SNotificationItem::CS_Pending;
			break;

		case EMCPServerStatus::Offline:
		default:
			Message = LOCTEXT("MCPOfflineMessage", "MCP Server Offline\n\nCheck the Output Log for errors.\nMake sure the plugin is enabled and ServerEnabled=true in config.");
			State = SNotificationItem::CS_Fail;
			
			// Attempt to restart the server
			if (Server.IsValid() && !Server->IsRunning())
			{
				UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Attempting to restart MCP server..."));
				if (Server->StartServer(8767))
				{
					FPlatformApplicationMisc::ClipboardCopy(*ConfigJson);
					Message = LOCTEXT("MCPRestartedMessage", "MCP server restarted successfully!\n\nConfiguration copied to clipboard.");
					State = SNotificationItem::CS_Success;
				}
			}
			break;
	}

	// Show notification
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.ExpireDuration = 5.0f;
	Info.bUseSuccessFailIcons = true;

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(State);
	}

	return FReply::Handled();
}

EActiveTimerReturnType SMCPStatusBarWidget::UpdateStatus(double InCurrentTime, float InDeltaTime)
{
	EMCPServerStatus NewStatus = GetServerStatus();
	
	TSharedPtr<FSpecialAgentMCPServer> Server = MCPServer.Pin();
	if (Server.IsValid())
	{
		ConnectedClients = Server->GetConnectedClientCount();
	}
	else
	{
		ConnectedClients = 0;
	}

	// Log status changes
	if (NewStatus != CachedStatus)
	{
		switch (NewStatus)
		{
			case EMCPServerStatus::Connected:
				UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP client connected"));
				break;
			case EMCPServerStatus::Listening:
				UE_LOG(LogTemp, Log, TEXT("SpecialAgent: MCP server listening"));
				break;
			case EMCPServerStatus::Offline:
				UE_LOG(LogTemp, Warning, TEXT("SpecialAgent: MCP server went offline"));
				break;
		}
	}

	CachedStatus = NewStatus;

	// Continue the timer
	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE

