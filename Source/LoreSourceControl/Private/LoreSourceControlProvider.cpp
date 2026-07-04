#include "LoreSourceControlProvider.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Text/STextBlock.h"
#include "SLoreSettings.h"
#include "LoreSourceControlState.h"
#include "SourceControlOperations.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformTime.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "ISourceControlModule.h"
#include "SourceControlWindows.h"
#include <signal.h>
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"


static FString NormalizeLorePath(const FString& InPath)
{
    FString Abs = FPaths::ConvertRelativePathToFull(InPath);
    FPaths::NormalizeFilename(Abs);
    return Abs;
}

FLoreSourceControlProvider::FLoreSourceControlProvider()
    : ProviderName("Lore")
    , bIsConnected(false)
    , StatusMessage("Not connected")
{
}

const FName& FLoreSourceControlProvider::GetName() const
{
    return ProviderName;
}

void FLoreSourceControlProvider::ShowReconnectNotification()
{
    // Show a toast notification in the Editor
    FNotificationInfo Info(FText::FromString(
        TEXT("Lore Source Control: Not connected. Click to reconnect.")));
    Info.ExpireDuration = 10.f;
    Info.bUseSuccessFailIcons = true;
    Info.bUseLargeFont = false;

    // Add a reconnect button to the notification
    Info.ButtonDetails.Add(FNotificationButtonInfo(
        NSLOCTEXT("LoreSC", "ReconnectBtn", "Reconnect"),
        NSLOCTEXT("LoreSC", "ReconnectTip", "Open source control settings"),
        FSimpleDelegate::CreateLambda([]()
        {
            // Open the source control login dialog
            ISourceControlModule::Get().ShowLoginDialog(
                FSourceControlLoginClosed(),
                ELoginWindowMode::Modeless
            );
        })
    ));

    TSharedPtr<SNotificationItem> Notification =
        FSlateNotificationManager::Get().AddNotification(Info);

    if (Notification.IsValid())
    {
        Notification->SetCompletionState(SNotificationItem::CS_Fail);
    }
}

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
    // Load saved settings
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("ServerUrl"), ServerUrl, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("RepositoryName"), RepositoryName, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("Identity"), Identity, GEditorSettingsIni);

    if (ServerUrl.IsEmpty())
    {
        StatusMessage = TEXT("Not connected. Configure settings and click Connect.");
        bIsConnected = false;
        return;
    }

    // Don't run lore commands here — FPaths::ProjectDir() may not be
    // fully resolved this early during module startup. Just mark as
    // "pending" and let Tick() do the real check once the engine has
    // fully finished loading.
    bIsConnected = false;
    StatusMessage = TEXT("Checking connection...");
    bPendingStartupCheck = true;
}

void FLoreSourceControlProvider::Close()
{
    bIsConnected = false;
    StatusMessage = TEXT("Not connected");
}

FText FLoreSourceControlProvider::GetStatusText() const
{
    return FText::FromString(StatusMessage);
}

TMap<ISourceControlProvider::EStatus, FString>
FLoreSourceControlProvider::GetStatus() const
{
    TMap<EStatus, FString> StatusMap;
    StatusMap.Add(EStatus::Enabled,
        IsEnabled() ? TEXT("Yes") : TEXT("No"));
    StatusMap.Add(EStatus::Connected,
        IsAvailable() ? TEXT("Yes") : TEXT("No"));
    if (!ServerUrl.IsEmpty())
        StatusMap.Add(EStatus::User, ServerUrl);
    if (!RepositoryName.IsEmpty())
        StatusMap.Add(EStatus::Repository, RepositoryName);
    return StatusMap;
}

bool FLoreSourceControlProvider::IsEnabled() const   { return true; }
bool FLoreSourceControlProvider::IsAvailable() const
{
    return bIsConnected;
}
bool FLoreSourceControlProvider::QueryStateBranchConfig(
    const FString& ConfigSrc, const FString& ConfigDest) { return false; }

void FLoreSourceControlProvider::RegisterStateBranches(
    const TArray<FString>& BranchNames,
    const FString& ContentRoot) {}

int32 FLoreSourceControlProvider::GetStateBranchIndex(
    const FString& BranchName) const { return INDEX_NONE; }

bool FLoreSourceControlProvider::GetStateBranchAtIndex(
    int32 BranchIndex, FString& OutBranchName) const { return false; }




ECommandResult::Type FLoreSourceControlProvider::GetState(
    const TArray<FString>& InFiles,
    TArray<FSourceControlStateRef>& OutState,
    EStateCacheUsage::Type InStateCacheUsage)
{
    if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
    {
        UpdateStateCache(InFiles);
    }

    for (const FString& File : InFiles)
    {
        FString NormFile = NormalizeLorePath(File);
        TSharedRef<FLoreSourceControlState>* Cached = StateCache.Find(NormFile);
        if (Cached)
        {
            OutState.Add(*Cached);
        }
        else
        {
            // Not in cache yet — create unknown state
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(NormFile);
            State->bIsUnknown = false;
            OutState.Add(State);
        }
    }

    return ECommandResult::Succeeded;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(
    const TArray<FSourceControlChangelistRef>& InChangelists,
    TArray<FSourceControlChangelistStateRef>& OutState,
    EStateCacheUsage::Type InStateCacheUsage)
{
    return ECommandResult::Succeeded;
}

TArray<FSourceControlStateRef>
FLoreSourceControlProvider::GetCachedStateByPredicate(
    TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
    return {};
}

FDelegateHandle
FLoreSourceControlProvider::RegisterSourceControlStateChanged_Handle(
    const FSourceControlStateChanged::FDelegate& Delegate)
{
    return OnSourceControlStateChanged.Add(Delegate);
}

void FLoreSourceControlProvider::UnregisterSourceControlStateChanged_Handle(
    FDelegateHandle Handle)
{
    OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FLoreSourceControlProvider::Execute(
    const FSourceControlOperationRef& InOperation,
    FSourceControlChangelistPtr InChangelist,
    const TArray<FString>& InFiles,
    EConcurrency::Type InConcurrency,
    const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
    FName OpName = InOperation->GetName();

    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Execute: %s (Files: %d)"),
        *OpName.ToString(), InFiles.Num());
    for (const FString& F : InFiles)
    {
        UE_LOG(LogTemp, Log, TEXT("LoreSourceControl:   File: %s"), *F);
    }

    ECommandResult::Type Result = ECommandResult::Succeeded;

    if (OpName == "UpdateStatus")
    {
        UpdateStateCache(InFiles);
    }
    else if (OpName == "MarkForAdd")
    {
        for (const FString& File : InFiles)
        {
            FString AbsPath = File;
            if (File.StartsWith(TEXT("/Game/")))
            {
                AbsPath = FPaths::ProjectContentDir()
                        + File.Mid(6) + TEXT(".umap");
                if (!FPaths::FileExists(AbsPath))
                {
                    AbsPath = FPaths::ProjectContentDir()
                            + File.Mid(6) + TEXT(".uasset");
                }
            }

            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: MarkForAdd: %s"), *AbsPath);

            FString DirtyOutput = RunLoreCommand({ TEXT("dirty"), AbsPath });
            FString StageOutput = RunLoreCommand({ TEXT("stage"), AbsPath });
            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Stage: %s"), *StageOutput);

            // Update cache
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(File);
            State->bIsAdded   = true;
            State->bIsUnknown = false;
            State->TimeStamp  = FDateTime::Now();
            StateCache.Add(NormalizeLorePath(File), State);
        }

        OnSourceControlStateChanged.Broadcast();
    }
    else if (OpName == "CheckOut")
    {
        for (const FString& File : InFiles)
        {
            FString AbsPath = File;
            if (File.StartsWith(TEXT("/Game/")))
            {
                AbsPath = FPaths::ProjectContentDir()
                        + File.Mid(6) + TEXT(".umap");
                if (!FPaths::FileExists(AbsPath))
                {
                    AbsPath = FPaths::ProjectContentDir()
                            + File.Mid(6) + TEXT(".uasset");
                }
            }

            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: CheckOut: %s"), *AbsPath);

            // Step 1 — mark dirty so Lore tracks it
            FString DirtyOutput = RunLoreCommand({ TEXT("dirty"), AbsPath });
            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Dirty output: %s"), *DirtyOutput);

            // Step 2 — stage it
            FString StageOutput = RunLoreCommand({ TEXT("stage"), AbsPath });
            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Stage output: %s"), *StageOutput);

            // Step 3 — acquire exclusive lock
            FString LockOutput = RunLoreCommand({
                TEXT("lock"), TEXT("acquire"), AbsPath });
            UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Lock output: %s"), *LockOutput);

            // Update cache — mark as checked out
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(File);
            State->bIsModified = true;
            State->bIsUnknown  = false;
            State->bIsLocked   = true;
            State->TimeStamp   = FDateTime::Now();
            StateCache.Add(NormalizeLorePath(File), State);
        }

        OnSourceControlStateChanged.Broadcast();
    }

    else if (OpName == "CheckIn")
    {
        TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOp =
            StaticCastSharedRef<FCheckIn>(InOperation);
        FString Message = CheckInOp->GetDescription().ToString();
        if (Message.IsEmpty())
            Message = TEXT("Submitted via Unreal Editor");

        for (const FString& File : InFiles)
        {
            FString AbsPath = File;
            if (File.StartsWith(TEXT("/Game/")))
            {
                AbsPath = FPaths::ProjectContentDir()
                        + File.Mid(6) + TEXT(".umap");
                if (!FPaths::FileExists(AbsPath))
                {
                    AbsPath = FPaths::ProjectContentDir()
                            + File.Mid(6) + TEXT(".uasset");
                }
            }

            // Stage the file
            RunLoreCommand({ TEXT("dirty"), AbsPath });
            RunLoreCommand({ TEXT("stage"), AbsPath });

            // Release lock
            FString UnlockOutput = RunLoreCommand({
                TEXT("lock"), TEXT("release"), AbsPath });
            UE_LOG(LogTemp, Log,
                TEXT("LoreSourceControl: Unlock: %s"), *UnlockOutput);
        }

        // Commit
        FString CommitOutput = RunLoreCommand({ TEXT("commit"), Message, TEXT("--identity"), Identity });
        UE_LOG(LogTemp, Log,
            TEXT("LoreSourceControl: Commit: %s"), *CommitOutput);

        // Push
        FString PushOutput = RunLoreCommand({ TEXT("push"), TEXT("--identity"), Identity });
        UE_LOG(LogTemp, Log,
            TEXT("LoreSourceControl: Push: %s"), *PushOutput);

        // Update cache — all files now clean
        for (const FString& File : InFiles)
        {
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(File);
            State->bIsModified = false;
            State->bIsUnknown  = false;
            State->bIsLocked   = false;
            State->TimeStamp   = FDateTime::Now();
            StateCache.Add(NormalizeLorePath(File), State);
        }
        OnSourceControlStateChanged.Broadcast();
    }

    else if (OpName == "Revert")
    {
        for (const FString& File : InFiles)
        {
            FString AbsPath = File;
            if (File.StartsWith(TEXT("/Game/")))
            {
                AbsPath = FPaths::ProjectContentDir()
                        + File.Mid(6) + TEXT(".umap");
                if (!FPaths::FileExists(AbsPath))
                {
                    AbsPath = FPaths::ProjectContentDir()
                            + File.Mid(6) + TEXT(".uasset");
                }
            }

            // Make path relative to project dir for lore reset
            FString RelPath = AbsPath;
            FPaths::MakePathRelativeTo(RelPath, *FPaths::ProjectDir());

            UE_LOG(LogTemp, Log,
                TEXT("LoreSourceControl: Reverting rel: %s"), *RelPath);

            // Release lock
            RunLoreCommand({ TEXT("lock"), TEXT("release"), AbsPath });

            // Reset using relative path
            FString ResetOutput = RunLoreCommand({ TEXT("reset"), RelPath });
            UE_LOG(LogTemp, Log,
                TEXT("LoreSourceControl: Reset: %s"), *ResetOutput);

            // Update cache
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(File);
            State->bIsModified = false;
            State->bIsUnknown  = false;
            State->bIsLocked   = false;
            State->TimeStamp   = FDateTime::Now();
            StateCache.Add(NormalizeLorePath(File), State);
        }
        OnSourceControlStateChanged.Broadcast();
    }
    else if (OpName == "Sync")
    {
        UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Syncing..."));

        FString SyncOutput = RunLoreCommand({ TEXT("sync"), TEXT("--identity"), Identity });
        UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Sync: %s"), *SyncOutput);

        // Refresh all file states after sync
        UpdateStateCache(InFiles);
    }

    InOperationCompleteDelegate.ExecuteIfBound(InOperation, Result);
    return Result;
}

bool FLoreSourceControlProvider::CanExecuteOperation(
    const FSourceControlOperationRef& InOperation) const
{
    return true;
}

bool FLoreSourceControlProvider::CanCancelOperation(
    const FSourceControlOperationRef& InOperation) const { return false; }

void FLoreSourceControlProvider::CancelOperation(
    const FSourceControlOperationRef& InOperation) {}

TArray<TSharedRef<class ISourceControlLabel>>
FLoreSourceControlProvider::GetLabels(
    const FString& InMatchingSpec) const { return {}; }

TArray<FSourceControlChangelistRef>
FLoreSourceControlProvider::GetChangelists(
    EStateCacheUsage::Type InStateCacheUsage) { return {}; }

TOptional<bool> FLoreSourceControlProvider::HasChangesToSync() const
{
    return TOptional<bool>();
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToCheckIn() const
{
    return TOptional<bool>();
}

void FLoreSourceControlProvider::Tick()
{
    if (bPendingStartupCheck)
    {
        bPendingStartupCheck = false;

        FString StatusOutput = RunLoreCommand({ TEXT("status") });
        bool bLocalOk = StatusOutput.Contains(TEXT("Repository"))
                     && !StatusOutput.Contains(TEXT("Error"));

        if (!bLocalOk)
        {
            bIsConnected = false;
            StatusMessage = TEXT("Local repo not found. Please reconnect.");
            ShowReconnectNotification();
        }
        else
        {
            FString ServerCheck = RunLoreCommand({
                TEXT("repository"), TEXT("list"), ServerUrl,
                TEXT("--identity"), Identity
            });

            bool bRepoExists = ServerCheck.Contains(RepositoryName)
                            && !ServerCheck.Contains(TEXT("Error"));

            if (bRepoExists)
            {
                bIsConnected = true;
                StatusMessage = FString::Printf(
                    TEXT("Connected to %s/%s"), *ServerUrl, *RepositoryName);
            }
            else
            {
                bIsConnected = false;
                StatusMessage = FString::Printf(
                    TEXT("Repository '%s' not found on server. Please reconnect."),
                    *RepositoryName);
                ShowReconnectNotification();
            }
        }
    }

    // Refresh state every 30 seconds when connected
    if (bIsConnected)
    {
        static double LastRefreshTime = 0.0;
        double CurrentTime = FPlatformTime::Seconds();
        if (CurrentTime - LastRefreshTime > 30.0)
        {
            LastRefreshTime = CurrentTime;
            TArray<FString> CachedFiles;
            StateCache.GetKeys(CachedFiles);
            if (CachedFiles.Num() > 0)
            {
                UpdateStateCache(CachedFiles);
            }
        }
    }
}

TSharedRef<SWidget> FLoreSourceControlProvider::MakeSettingsWidget() const
{
    return SNew(SLoreSettings);
}

FString FLoreSourceControlProvider::RunLoreCommand(
    const TArray<FString>& Args) const
{
    FString AbsProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    UE_LOG(LogTemp, Warning, TEXT("LoreSourceControl: Using CWD: %s"), *AbsProjectDir);

    // Build the lore command line
    FString ArgString;
    for (int32 i = 0; i < Args.Num(); i++)
    {
        if (i > 0) ArgString += TEXT(" ");
        ArgString += TEXT("\"") + Args[i] + TEXT("\"");
    }

    // FPlatformProcess::CreateProc's WorkingDirectory param is unreliable
    // on Linux for this engine build, and nested quoting for `bash -c`
    // breaks UE's own command-line tokenizer. Write a temp script instead
    // — this avoids all quoting issues entirely.
    FString ScriptPath = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("lore_cmd.sh"));
    FString ScriptContent = FString::Printf(
        TEXT("#!/bin/bash\ncd \"%s\"\nlore %s\n"), *AbsProjectDir, *ArgString);

    FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath);

    // Make it executable
   

    void* ReadPipe  = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    FProcHandle ProcHandle = FPlatformProcess::CreateProc(
        TEXT("/bin/bash"), *ScriptPath,
        false, true, true,
        nullptr, 0,
        nullptr,
        WritePipe, ReadPipe
    );

    FString Output;
    if (ProcHandle.IsValid())
    {
        const double StartTime = FPlatformTime::Seconds();
        const double TimeoutSeconds = 30.0;

        while (FPlatformProcess::IsProcRunning(ProcHandle))
        {
            Output += FPlatformProcess::ReadPipe(ReadPipe);

            if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("LoreSourceControl: Command timed out: %s"), *ArgString);

                pid_t Pid = (pid_t)(intptr_t)ProcHandle.Get();
                if (Pid != 0)
                {
                    kill(Pid, SIGKILL);
                }
                FPlatformProcess::TerminateProc(ProcHandle, true);

                Output = TEXT("ERROR: timeout");
                break;
            }

            FPlatformProcess::Sleep(0.01f);
        }
        Output += FPlatformProcess::ReadPipe(ReadPipe);
        FPlatformProcess::CloseProc(ProcHandle);
    }
    else
    {
        Output = TEXT("ERROR: Could not launch lore. Is it on PATH?");
    }

    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
    IFileManager::Get().Delete(*ScriptPath);

    return Output.TrimStartAndEnd();
}
void FLoreSourceControlProvider::UpdateStateCache(const TArray<FString>& InFiles)
{
    FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));

    // --- Run lore lock query to get locked files ---
    TSet<FString> LockedFiles;
    FString LockOutput = RunLoreCommand({ TEXT("lock"), TEXT("query") });
    TArray<FString> LockLines;
    LockOutput.ParseIntoArrayLines(LockLines);

    for (const FString& Line : LockLines)
    {
        // Lines look like:
        // Projects/UnrealProjects/MyProject/Content/maps/map_2.umap by <unknown> on branch xxx
        if (Line.StartsWith(TEXT("Projects/")) || Line.StartsWith(TEXT("/")))
        {
            // Grab just the path part before " by "
            FString PathPart;
            if (Line.Split(TEXT(" by "), &PathPart, nullptr))
            {
                PathPart = PathPart.TrimStartAndEnd();
            }
            else
            {
                PathPart = Line.TrimStartAndEnd();
            }

            // Make absolute — Lore gives path relative to HOME
            FString AbsPath = FPaths::ConvertRelativePathToFull(HomeDir / PathPart);
            FPaths::NormalizeFilename(AbsPath);
            LockedFiles.Add(AbsPath);
        }
    }

    // --- Run lore status to get dirty/staged files ---
    TSet<FString> DirtyFiles;
    FString StatusOutput = RunLoreCommand({ TEXT("status") });
    TArray<FString> StatusLines;
    StatusOutput.ParseIntoArrayLines(StatusLines);

    bool bInStagedSection = false;
    for (const FString& Line : StatusLines)
    {
        if (Line.Contains(TEXT("staged for commit")))
        {
            bInStagedSection = true;
            continue;
        }
        if (Line.IsEmpty()) { bInStagedSection = false; continue; }

        if (bInStagedSection)
        {
            FString TrimmedLine = Line.TrimStartAndEnd();

            // Strip the leading status char + space ("M ", "A ", "D " etc)
            if (TrimmedLine.Len() > 2 && TrimmedLine[1] == TEXT(' '))
            {
                TrimmedLine = TrimmedLine.Mid(2).TrimStartAndEnd();
            }

            // Path is relative to project root
            FString AbsPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TrimmedLine);
            FPaths::NormalizeFilename(AbsPath);
            DirtyFiles.Add(AbsPath);
        }
    }

    // --- Update StateCache for every requested file ---
    for (const FString& File : InFiles)
    {
        FString NormFile = NormalizeLorePath(File);
        TSharedRef<FLoreSourceControlState> State =
            MakeShared<FLoreSourceControlState>(NormFile);

        if (LockedFiles.Contains(NormFile))
        {
            State->bIsLocked   = true;
            State->bIsModified = true;
            State->bIsUnknown  = false;
        }
        else if (DirtyFiles.Contains(NormFile))
        {
            State->bIsModified = true;
            State->bIsUnknown  = false;
        }
        else
        {
            State->bIsModified = false;
            State->bIsLocked   = false;
            State->bIsUnknown  = false;
        }

        State->TimeStamp = FDateTime::Now();
        StateCache.Add(NormFile, State);
    }

    OnSourceControlStateChanged.Broadcast();
}