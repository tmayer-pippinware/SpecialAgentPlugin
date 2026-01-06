// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/ScreenshotService.h"
#include "GameThreadDispatcher.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"

FScreenshotService::FScreenshotService()
{
}

FString FScreenshotService::GetServiceDescription() const
{
	return TEXT("Screenshot capture - CRITICAL visual feedback for iterative design");
}

TArray<FMCPToolInfo> FScreenshotService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;
	
	// capture
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("capture");
		Tool.Description = TEXT("ALWAYS USE FIRST. Returns viewport image. Then estimate % positions (0-1) for trace_from_screen or select_at_screen. Example: object at image center = (0.5, 0.5), object 1/4 from left and 3/4 down = (0.25, 0.75). Use BEFORE actions to plan, AFTER actions to verify.");
		
		TSharedPtr<FJsonObject> WidthParam = MakeShared<FJsonObject>();
		WidthParam->SetStringField(TEXT("type"), TEXT("number"));
		WidthParam->SetStringField(TEXT("description"), TEXT("Image width in pixels (default: 1280)"));
		Tool.Parameters->SetObjectField(TEXT("width"), WidthParam);
		
		TSharedPtr<FJsonObject> HeightParam = MakeShared<FJsonObject>();
		HeightParam->SetStringField(TEXT("type"), TEXT("number"));
		HeightParam->SetStringField(TEXT("description"), TEXT("Image height in pixels (default: 720)"));
		Tool.Parameters->SetObjectField(TEXT("height"), HeightParam);
		
		TSharedPtr<FJsonObject> QualityParam = MakeShared<FJsonObject>();
		QualityParam->SetStringField(TEXT("type"), TEXT("number"));
		QualityParam->SetStringField(TEXT("description"), TEXT("JPEG quality 1-99, or 100 for lossless PNG (default: 85)"));
		Tool.Parameters->SetObjectField(TEXT("quality"), QualityParam);
		
		Tools.Add(Tool);
	}
	
	// save
	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("save");
		Tool.Description = TEXT("Capture viewport screenshot and save to file.");
		
		TSharedPtr<FJsonObject> FileParam = MakeShared<FJsonObject>();
		FileParam->SetStringField(TEXT("type"), TEXT("string"));
		FileParam->SetStringField(TEXT("description"), TEXT("File path to save screenshot"));
		Tool.Parameters->SetObjectField(TEXT("file_path"), FileParam);
		Tool.RequiredParams.Add(TEXT("file_path"));
		
		Tools.Add(Tool);
	}
	
	return Tools;
}

FMCPResponse FScreenshotService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("capture")) return HandleCapture(Request);
	if (MethodName == TEXT("save")) return HandleSave(Request);

	return MethodNotFound(Request.Id, TEXT("screenshot"), MethodName);
}

FMCPResponse FScreenshotService::HandleCapture(const FMCPRequest& Request)
{
	// Get parameters - smaller defaults to avoid huge base64 strings causing client hangs
	int32 Width = 1280;
	int32 Height = 720;
	int32 Quality = 85;  // JPEG quality (1-100), use 100 for PNG
	bool bReturnBase64 = true;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetNumberField(TEXT("width"), Width);
		Request.Params->TryGetNumberField(TEXT("height"), Height);
		Request.Params->TryGetNumberField(TEXT("quality"), Quality);
		Request.Params->TryGetBoolField(TEXT("return_base64"), bReturnBase64);
	}
	
	// Clamp quality to valid range
	Quality = FMath::Clamp(Quality, 1, 100);

	// Capture on game thread
	auto CaptureTask = [Width, Height, Quality, bReturnBase64]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Get the active viewport
		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport found"));
			return Result;
		}

		// Get viewport client
		FViewportClient* ViewportClient = Viewport->GetClient();
		if (!ViewportClient)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No viewport client found"));
			return Result;
		}

		// Read pixels from viewport
		TArray<FColor> Bitmap;
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		
		// Read the viewport pixels
		if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), FIntRect(0, 0, ViewportSize.X, ViewportSize.Y)))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to read viewport pixels"));
			return Result;
		}

		// Resize if requested size is different
		if (Width != ViewportSize.X || Height != ViewportSize.Y)
		{
			TArray<FColor> ResizedBitmap;
			FImageUtils::ImageResize(ViewportSize.X, ViewportSize.Y, Bitmap, Width, Height, ResizedBitmap, false);
			Bitmap = MoveTemp(ResizedBitmap);
			ViewportSize = FIntPoint(Width, Height);
		}

		// Use JPEG for smaller file sizes (quality < 100), PNG for lossless (quality = 100)
		EImageFormat ImageFormat = (Quality < 100) ? EImageFormat::JPEG : EImageFormat::PNG;
		FString MimeType = (Quality < 100) ? TEXT("image/jpeg") : TEXT("image/png");
		
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

		if (!ImageWrapper.IsValid())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to create image wrapper"));
			return Result;
		}

		// Set raw data
		if (!ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), ViewportSize.X, ViewportSize.Y, ERGBFormat::BGRA, 8))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to set raw image data"));
			return Result;
		}

		// Get compressed data
		TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(Quality);

		if (CompressedData.Num() == 0)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to compress image"));
			return Result;
		}

		// Encode to base64 if requested
		if (bReturnBase64)
		{
			FString Base64String = FBase64::Encode(CompressedData.GetData(), CompressedData.Num());
			Result->SetStringField(TEXT("base64_data"), Base64String);
			Result->SetStringField(TEXT("mimeType"), MimeType);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("width"), ViewportSize.X);
		Result->SetNumberField(TEXT("height"), ViewportSize.Y);
		Result->SetNumberField(TEXT("quality"), Quality);
		Result->SetNumberField(TEXT("data_size"), CompressedData.Num());

		UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screenshot captured: %dx%d, quality=%d, %lld bytes"), 
			ViewportSize.X, ViewportSize.Y, Quality, CompressedData.Num());

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(CaptureTask);

	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FScreenshotService::HandleSave(const FMCPRequest& Request)
{
	// Get parameters
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString FilePath;
	if (!Request.Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'file_path'"));
	}

	int32 Width = 1920;
	int32 Height = 1080;
	Request.Params->TryGetNumberField(TEXT("width"), Width);
	Request.Params->TryGetNumberField(TEXT("height"), Height);

	// Capture and save on game thread
	auto SaveTask = [Width, Height, FilePath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Get the active viewport
		FViewport* Viewport = GEditor->GetActiveViewport();
		if (!Viewport)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("No active viewport found"));
			return Result;
		}

		// Read pixels
		TArray<FColor> Bitmap;
		FIntPoint ViewportSize = Viewport->GetSizeXY();
		
		if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), FIntRect(0, 0, ViewportSize.X, ViewportSize.Y)))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to read viewport pixels"));
			return Result;
		}

		// Resize if needed
		if (Width != ViewportSize.X || Height != ViewportSize.Y)
		{
			TArray<FColor> ResizedBitmap;
			FImageUtils::ImageResize(ViewportSize.X, ViewportSize.Y, Bitmap, Width, Height, ResizedBitmap, false);
			Bitmap = MoveTemp(ResizedBitmap);
			ViewportSize = FIntPoint(Width, Height);
		}

		// Convert to PNG
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

		if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), ViewportSize.X, ViewportSize.Y, ERGBFormat::BGRA, 8))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to prepare image"));
			return Result;
		}

		TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(100);

		// Save to file
		if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("file_path"), FilePath);
			Result->SetNumberField(TEXT("width"), ViewportSize.X);
			Result->SetNumberField(TEXT("height"), ViewportSize.Y);
			Result->SetNumberField(TEXT("file_size"), CompressedData.Num());

			UE_LOG(LogTemp, Log, TEXT("SpecialAgent: Screenshot saved to: %s (%dx%d)"), 
				*FilePath, ViewportSize.X, ViewportSize.Y);
		}
		else
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to save file: %s"), *FilePath));
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(SaveTask);

	return FMCPResponse::Success(Request.Id, Result);
}

