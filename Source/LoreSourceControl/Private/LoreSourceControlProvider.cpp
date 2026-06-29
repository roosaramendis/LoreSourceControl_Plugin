#include "LoreSourceControlProvider.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Text/STextBlock.h"
#include "SLoreSettings.h"
#include "LoreSourceControlState.h"
#include "SourceControlOperations.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformTime.h"

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

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
     // Load saved settings
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("ServerUrl"), ServerUrl, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("RepositoryName"), RepositoryName, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("Identity"), Identity, GEditorSettingsIni);

    // Auto-connect if we have saved settings
    if (!ServerUrl.IsEmpty() && !RepositoryName.IsEmpty())
    {
        FString Output = RunLoreCommand({ TEXT("status") });
        bool bSuccess = Output.Contains(TEXT("Repository"))
                     && !Output.Contains(TEXT("Error"));

        if (bSuccess)
        {
            bIsConnected = true;
            StatusMessage = FString::Printf(
                TEXT("Connected to %s/%s"), *ServerUrl, *RepositoryName);
            UE_LOG(LogTemp, Log,
                TEXT("LoreSourceControl: Auto-connected to %s/%s"),
                *ServerUrl, *RepositoryName);
        }
    }
    else
    {
        StatusMessage = TEXT("Not connected. Configure settings and click Connect.");
    }
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
//bool FLoreSourceControlProvider::IsAvailable() const { return bIsConnected; }
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
        TSharedRef<FLoreSourceControlState>* Cached = StateCache.Find(File);
        if (Cached)
        {
            OutState.Add(*Cached);
        }
        else
        {
            // Not in cache yet — create unknown state
            TSharedRef<FLoreSourceControlState> State =
                MakeShared<FLoreSourceControlState>(File);
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

    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Execute: %s"),
        *OpName.ToString());

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
            StateCache.Add(File, State);
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
            StateCache.Add(File, State);
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
        FString CommitOutput = RunLoreCommand({ TEXT("commit"), Message });
        UE_LOG(LogTemp, Log,
            TEXT("LoreSourceControl: Commit: %s"), *CommitOutput);

        // Push
        FString PushOutput = RunLoreCommand({ TEXT("push") });
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
            StateCache.Add(File, State);
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
            StateCache.Add(File, State);
        }
        OnSourceControlStateChanged.Broadcast();
    }
    else if (OpName == "Sync")
    {
        UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Syncing..."));

        FString SyncOutput = RunLoreCommand({ TEXT("sync") });
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
    //return IsAvailable();
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
    // Refresh state every 30 seconds when connected
    if (bIsConnected)
    {
        static double LastRefreshTime = 0.0;
        double CurrentTime = FPlatformTime::Seconds();
        if (CurrentTime - LastRefreshTime > 30.0)
        {
            LastRefreshTime = CurrentTime;
            // Refresh cached states
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
    FString ArgString;
    for (int32 i = 0; i < Args.Num(); i++)
    {
        if (i > 0) ArgString += TEXT(" ");
        ArgString += TEXT("\"") + Args[i] + TEXT("\"");
    }

    void* ReadPipe  = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    FProcHandle ProcHandle = FPlatformProcess::CreateProc(
        TEXT("lore"), *ArgString,
        false, true, true,
        nullptr, 0,
        *FPaths::ProjectDir(),  // ← add this, was nullptr before
        WritePipe, ReadPipe
    );

    FString Output;
    if (ProcHandle.IsValid())
    {
        while (FPlatformProcess::IsProcRunning(ProcHandle))
        {
            Output += FPlatformProcess::ReadPipe(ReadPipe);
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
    return Output.TrimStartAndEnd();
}
void FLoreSourceControlProvider::UpdateStateCache(
    const TArray<FString>& InFiles)
{
    FString StatusOutput = RunLoreCommand({ TEXT("status") });

    TArray<FString> Lines;
    StatusOutput.ParseIntoArrayLines(Lines);

    // Parse status lines like "M Content/maps/map_1.umap"
    TMap<FString, TCHAR> StatusMap;
    for (const FString& Line : Lines)
    {
        if (Line.Len() < 3) continue;
        TCHAR StatusChar = Line[0];
        if (StatusChar != 'M' && StatusChar != 'A' && StatusChar != 'D')
            continue;
        FString RelPath = Line.Mid(2).TrimStartAndEnd();
        FString AbsPath = FPaths::ConvertRelativePathToFull(
            FPaths::ProjectDir() / RelPath);
        StatusMap.Add(AbsPath, StatusChar);
    }

    // Update cache for each requested file
    for (const FString& File : InFiles)
    {
        TSharedRef<FLoreSourceControlState> State =
            MakeShared<FLoreSourceControlState>(File);
        State->TimeStamp  = FDateTime::Now();
        State->bIsUnknown = true;

        const TCHAR* Found = StatusMap.Find(File);
        if (Found)
        {
            State->bIsUnknown  = false;
            State->bIsModified = (*Found == 'M');
            State->bIsAdded    = (*Found == 'A');
            State->bIsDeleted  = (*Found == 'D');
        }
        else
        {
            // File is tracked and clean
            State->bIsUnknown  = false;
            State->bIsModified = false;
            State->bIsAdded    = false;
            State->bIsDeleted  = false;
        }

        StateCache.Add(File, State);
    }

    OnSourceControlStateChanged.Broadcast();
}