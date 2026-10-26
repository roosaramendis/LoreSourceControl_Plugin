#include "LoreSourceControlProvider.h"

#include "LoreCommand.h"
#include "LoreSourceControlState.h"
#include "SLoreSettings.h"

#include "ISourceControlLabel.h"
#include "SourceControlHelpers.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ScopedSourceControlProgress.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Logging/MessageLog.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "CoreGlobals.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

namespace LoreProviderLocal
{
	static const TCHAR* SettingsSection = TEXT("LoreSourceControl.LoreSourceControlSettings");
	static const TCHAR* LegacySection = TEXT("LoreSourceControl");
	static constexpr double BackgroundRefreshSeconds = 60.0;

	// "lore://host:port/REPO" -> "lore://host:port"
	static FString ServerPart(const FString& Url)
	{
		const int32 SchemeEnd = Url.Find(TEXT("://"));
		if (SchemeEnd == INDEX_NONE)
		{
			return Url;
		}
		const int32 PathStart = Url.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeEnd + 3);
		return PathStart == INDEX_NONE ? Url : Url.Left(PathStart);
	}

	static bool IsSupported(const FName& Name)
	{
		return Name == LoreOpNames::Connect || Name == LoreOpNames::UpdateStatus || Name == LoreOpNames::CheckOut
			|| Name == LoreOpNames::CheckIn || Name == LoreOpNames::MarkForAdd || Name == LoreOpNames::Delete
			|| Name == LoreOpNames::Revert || Name == LoreOpNames::Sync;
	}
}

FLoreSourceControlProvider::FLoreSourceControlProvider()
	: ProviderName(TEXT("Lore"))
	, LockStore(MakeShared<FLoreLockStore, ESPMode::ThreadSafe>())
	, MoveStore(MakeShared<FLoreMoveStore, ESPMode::ThreadSafe>())
{
}

FLoreSourceControlProvider::~FLoreSourceControlProvider()
{
	Close();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FLoreSourceControlProvider::LoadSettings()
{
	using namespace LoreProviderLocal;
	const FString& Ini = SourceControlHelpers::GetSettingsIni();

	const bool bHaveNew = GConfig->GetString(SettingsSection, TEXT("ServerUrl"), Settings.ServerUrl, Ini);
	GConfig->GetString(SettingsSection, TEXT("RepositoryName"), Settings.RepositoryName, Ini);
	GConfig->GetString(SettingsSection, TEXT("Identity"), Settings.Identity, Ini);
	GConfig->GetString(SettingsSection, TEXT("LoreBinary"), Settings.LoreBinary, Ini);

	if (!bHaveNew)
	{
		// One-time move from where the old plugin version saved them.
		GConfig->GetString(LegacySection, TEXT("ServerUrl"), Settings.ServerUrl, GEditorSettingsIni);
		GConfig->GetString(LegacySection, TEXT("RepositoryName"), Settings.RepositoryName, GEditorSettingsIni);
		GConfig->GetString(LegacySection, TEXT("Identity"), Settings.Identity, GEditorSettingsIni);
		if (!Settings.ServerUrl.IsEmpty() || !Settings.Identity.IsEmpty())
		{
			SaveSettings();
		}
	}
}

void FLoreSourceControlProvider::SaveSettings() const
{
	using namespace LoreProviderLocal;
	const FString& Ini = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(SettingsSection, TEXT("ServerUrl"), *Settings.ServerUrl, Ini);
	GConfig->SetString(SettingsSection, TEXT("RepositoryName"), *Settings.RepositoryName, Ini);
	GConfig->SetString(SettingsSection, TEXT("Identity"), *Settings.Identity, Ini);
	GConfig->SetString(SettingsSection, TEXT("LoreBinary"), *Settings.LoreBinary, Ini);
	GConfig->Flush(false, Ini);
}

void FLoreSourceControlProvider::SetSettings(const FLoreSettings& InSettings, bool bSave)
{
	Settings = InSettings;
	Settings.ServerUrl.TrimStartAndEndInline();
	Settings.RepositoryName.TrimStartAndEndInline();
	Settings.Identity.TrimStartAndEndInline();
	Settings.LoreBinary.TrimStartAndEndInline();
	if (bSave)
	{
		SaveSettings();
	}
}

FLoreContext FLoreSourceControlProvider::MakeContext() const
{
	FLoreContext Ctx;
	Ctx.LoreBinary = Settings.LoreBinary.IsEmpty() ? LoreUtils::DefaultLoreBinary() : Settings.LoreBinary;
	Ctx.RepoRoot = RepoRoot;
	Ctx.Identity = !Settings.Identity.IsEmpty() ? Settings.Identity : ConfigIdentity;
	// The repository's own remote is the truth once it exists on disk.
	Ctx.ServerUrl = (bRepoFound && !ConfigRemoteUrl.IsEmpty())
		? LoreProviderLocal::ServerPart(ConfigRemoteUrl)
		: Settings.ServerUrl;
	Ctx.RepositoryName = Settings.RepositoryName;
	return Ctx;
}

void FLoreSourceControlProvider::RefreshRepoRoot()
{
	const FString ProjectDir = LoreUtils::NormalizePath(FPaths::ProjectDir());
	const FString Found = LoreUtils::FindRepoRoot(ProjectDir);
	const FString NewRoot = Found.IsEmpty() ? ProjectDir : Found;
	const bool bNewFound = !Found.IsEmpty();

	if (NewRoot != RepoRoot || bNewFound != bRepoFound)
	{
		StateCache.Reset(); // cached "outside the repo" answers are wrong now
	}
	RepoRoot = NewRoot;
	bRepoFound = bNewFound;
	ConfigRemoteUrl = bRepoFound ? LoreUtils::ReadConfigValue(RepoRoot, TEXT("remote_url")) : FString();
	ConfigIdentity = bRepoFound ? LoreUtils::ReadConfigValue(RepoRoot, TEXT("identity")) : FString();
}

void FLoreSourceControlProvider::StartConnectCheck()
{
	if (bConnectInFlight)
	{
		return;
	}
	Execute(ISourceControlOperation::Create<FConnect>(), FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous);
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

const FName& FLoreSourceControlProvider::GetName() const
{
	return ProviderName;
}

void FLoreSourceControlProvider::Init(bool bForceConnection)
{
	LoadSettings();
	RefreshRepoRoot();
	LockStore->SetFile(FPaths::ProjectSavedDir() / TEXT("LoreSourceControl") / TEXT("MyLocks.txt"));
	MoveStore->SetFile(FPaths::ProjectSavedDir() / TEXT("LoreSourceControl") / TEXT("PendingMoves.txt"));

	// Unreal never tells revision control about renames, so listen for them
	// ourselves and turn them into `lore stage move` on Mark For Add / Submit.
	if (!AssetRenamedHandle.IsValid())
	{
		FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRenamedHandle = AssetRegistry.Get().OnAssetRenamed().AddRaw(this, &FLoreSourceControlProvider::HandleAssetRenamed);
	}

	// Never block editor startup: the connection check runs in the background
	// and Tick() picks up the answer.
	if (bRepoFound)
	{
		StartConnectCheck();
	}
}

void FLoreSourceControlProvider::Close()
{
	if (AssetRenamedHandle.IsValid())
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
		{
			FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnAssetRenamed().Remove(AssetRenamedHandle);
		}
		AssetRenamedHandle.Reset();
	}
	if (WorkerPool)
	{
		WorkerPool->Destroy(); // waits for the running command, abandons queued ones
		delete WorkerPool;
		WorkerPool = nullptr;
	}
	for (FLoreCommand* Command : CommandQueue)
	{
		delete Command;
	}
	CommandQueue.Reset();
	StateCache.Reset();
	bAvailable = false;
	bConnectInFlight = false;
	bRefreshInFlight = false;
	bHasConnectedOnce = false;
}

void FLoreSourceControlProvider::HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	if (!bRepoFound)
	{
		return;
	}
	const FString NewPackage = AssetData.PackageName.ToString();
	const FString OldPackage = FPackageName::ObjectPathToPackageName(OldObjectPath);
	if (OldPackage.IsEmpty() || OldPackage == NewPackage)
	{
		return; // renamed inside the same package: no file moved
	}
	const FString Extension = (AssetData.PackageFlags & PKG_ContainsMap) ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
	FString NewFile, OldFile;
	if (!FPackageName::TryConvertLongPackageNameToFilename(NewPackage, NewFile, Extension)
		|| !FPackageName::TryConvertLongPackageNameToFilename(OldPackage, OldFile, Extension))
	{
		return;
	}
	FString NewRel, OldRel;
	if (!LoreUtils::ToRelative(RepoRoot, LoreUtils::NormalizePath(NewFile), NewRel)
		|| !LoreUtils::ToRelative(RepoRoot, LoreUtils::NormalizePath(OldFile), OldRel))
	{
		return;
	}

	// A -> B then B -> C is one move A -> C. Moving back to A cancels it.
	FString Original;
	if (MoveStore->Find(OldRel, Original))
	{
		MoveStore->Remove(OldRel);
		OldRel = Original;
	}
	if (OldRel == NewRel)
	{
		return;
	}
	MoveStore->Add(NewRel, OldRel);
	UE_LOG(LogLoreSC, Log, TEXT("Rename noted: %s -> %s (recorded as a Lore move on submit)"), *OldRel, *NewRel);
}

FQueuedThreadPool* FLoreSourceControlProvider::GetWorkerPool()
{
	if (!WorkerPool)
	{
		// ONE thread: lore commands for this repo always run one after another.
		WorkerPool = FQueuedThreadPool::Allocate();
		WorkerPool->Create(1, 256 * 1024, TPri_Normal, TEXT("LoreSourceControl"));
	}
	return WorkerPool;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

FText FLoreSourceControlProvider::GetStatusText() const
{
	const FLoreContext Ctx = MakeContext();
	FFormatNamedArguments Args;
	Args.Add(TEXT("Server"), FText::FromString(Ctx.ServerUrl.IsEmpty() ? FString(TEXT("(not set)")) : Ctx.ServerUrl));
	Args.Add(TEXT("Identity"), FText::FromString(Ctx.Identity.IsEmpty() ? FString(TEXT("(not set)")) : Ctx.Identity));
	Args.Add(TEXT("Repo"), FText::FromString(bRepoFound ? RepoRoot : FString(TEXT("(no .lore folder found)"))));
	Args.Add(TEXT("Branch"), FText::FromString(BranchName.IsEmpty() ? FString(TEXT("?")) : BranchName));
	Args.Add(TEXT("Local"), FText::FromString(LocalRevision.IsSet() ? FString::FromInt(LocalRevision.GetValue()) : FString(TEXT("?"))));
	Args.Add(TEXT("Remote"), FText::FromString(RemoteRevision.IsSet() ? FString::FromInt(RemoteRevision.GetValue()) : FString(TEXT("?"))));
	Args.Add(TEXT("Connected"), bAvailable ? LOCTEXT("Yes", "yes") : LOCTEXT("No", "no"));
	return FText::Format(LOCTEXT("StatusText",
		"Server: {Server}\nIdentity: {Identity}\nRepository: {Repo}\nBranch: {Branch} (local revision {Local}, server revision {Remote})\nConnected: {Connected}"), Args);
}

bool FLoreSourceControlProvider::IsEnabled() const
{
	return true;
}

bool FLoreSourceControlProvider::IsAvailable() const
{
	return bAvailable;
}

TMap<ISourceControlProvider::EStatus, FString> FLoreSourceControlProvider::GetStatus() const
{
	const FLoreContext Ctx = MakeContext();
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, IsAvailable() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::User, Ctx.Identity);
	Result.Add(EStatus::Repository, Ctx.ServerUrl + TEXT(" ") + RepoRoot);
	return Result;
}

TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> FLoreSourceControlProvider::GetStateInternal(const FString& AbsPath)
{
	if (const TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>* Existing = StateCache.Find(AbsPath))
	{
		return *Existing;
	}
	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = MakeShared<FLoreSourceControlState, ESPMode::ThreadSafe>(AbsPath);
	FString Rel;
	if (!bRepoFound || (AbsPath != RepoRoot && !LoreUtils::ToRelative(RepoRoot, AbsPath, Rel)))
	{
		State->WorkingState = ELoreWorkingState::Outside;
	}
	StateCache.Add(AbsPath, State);
	return State;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	TArray<FString> AbsFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);
	for (FString& File : AbsFiles)
	{
		File = LoreUtils::NormalizePath(File);
	}

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate && bRepoFound)
	{
		// The base-class Execute() overloads are hidden by our override,
		// so call the full signature.
		Execute(ISourceControlOperation::Create<FUpdateStatus>(), FSourceControlChangelistPtr(), AbsFiles, EConcurrency::Synchronous);
	}

	for (const FString& File : AbsFiles)
	{
		OutState.Add(GetStateInternal(File));
	}
	return ECommandResult::Succeeded;
}

ECommandResult::Type FLoreSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	return ECommandResult::Failed; // no changelists in Lore
}

TArray<FSourceControlStateRef> FLoreSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
	{
		FSourceControlStateRef State = Pair.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

FDelegateHandle FLoreSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FLoreSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

bool FLoreSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return LoreProviderLocal::IsSupported(InOperation->GetName());
}

ECommandResult::Type FLoreSourceControlProvider::Execute(
	const FSourceControlOperationRef& InOperation,
	FSourceControlChangelistPtr InChangelist,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	const FName OpName = InOperation->GetName();

	auto Fail = [&](const FText& Message)
	{
		InOperation->AddErrorMessge(Message);
		FMessageLog(TEXT("SourceControl")).Error(Message);
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	};

	if (!LoreProviderLocal::IsSupported(OpName))
	{
		return Fail(FText::Format(LOCTEXT("Unsupported", "Lore does not support the '{0}' operation yet."), FText::FromName(OpName)));
	}

	if (OpName == LoreOpNames::Connect)
	{
		RefreshRepoRoot(); // the user may have just created/cloned the repo
	}
	else if (!bRepoFound)
	{
		return Fail(FText::Format(LOCTEXT("NoRepo", "No Lore repository (.lore folder) found for {0}. Open Revision Control settings and use 'Set Up Repository'."),
			FText::FromString(RepoRoot)));
	}

	TArray<FString> AbsFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);
	for (FString& File : AbsFiles)
	{
		File = LoreUtils::NormalizePath(File);
	}

	FLoreCommand* Command = new FLoreCommand(InOperation, AbsFiles, MakeContext(), LockStore, InOperationCompleteDelegate);
	Command->MoveStore = MoveStore;
	if (OpName == LoreOpNames::CheckIn)
	{
		TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = StaticCastSharedRef<FCheckIn>(InOperation);
		Command->CheckInMessage = CheckIn->GetDescription().ToString();
		Command->bKeepCheckedOut = CheckIn->GetKeepCheckedOut();
	}
	else if (OpName == LoreOpNames::Revert)
	{
		Command->bSoftRevert = StaticCastSharedRef<FRevert>(InOperation)->IsSoftRevert();
	}
	else if (OpName == LoreOpNames::Connect)
	{
		bConnectInFlight = true;
	}
	else if (OpName == LoreOpNames::Sync)
	{
		Command->SyncRevision = StaticCastSharedRef<FSync>(InOperation)->GetRevision();
		if (Command->SyncRevision.IsEmpty() && !ChooseSyncResolutions(*Command))
		{
			delete Command;
			InOperation->AddInfoMessge(LOCTEXT("SyncCancelled", "Sync cancelled. Nothing was changed."));
			InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Cancelled);
			return ECommandResult::Cancelled;
		}
	}
	else if (OpName == LoreOpNames::UpdateStatus)
	{
		Command->bUpdateHistory = StaticCastSharedRef<FUpdateStatus>(InOperation)->ShouldUpdateHistory();
	}

	if (InConcurrency == EConcurrency::Synchronous)
	{
		// The Cancel button only appears for operations that can stop safely.
		FSimpleDelegate OnCancel;
		if (Command->IsCancellable())
		{
			OnCancel = FSimpleDelegate::CreateLambda([Command]()
			{
				if (Command->IsCancellable())
				{
					Command->bCancelRequested = true;
				}
			});
		}
		FScopedSourceControlProgress Progress(InOperation->GetInProgressString(), OnCancel);
		SynchronousCommand = Command;
		GetWorkerPool()->AddQueuedWork(Command);
		while (!Command->bWorkDone)
		{
			FPlatformProcess::Sleep(0.01f);
			Progress.Tick();
		}
		SynchronousCommand = nullptr;
		// Older async commands ran before this one (single thread, FIFO).
		// Apply their results FIRST so they can't overwrite our newer state.
		ProcessFinishedQueue();
		const ECommandResult::Type Result = ProcessCompletedCommand(*Command);
		delete Command;
		return Result;
	}

	CommandQueue.Add(Command);
	GetWorkerPool()->AddQueuedWork(Command);
	return ECommandResult::Succeeded;
}

bool FLoreSourceControlProvider::ChooseSyncResolutions(FLoreCommand& Command) const
{
	// Changed here (Modified) AND newer on the server (out of date).
	// Text files are merged automatically, so only assets need an answer.
	TArray<FString> Assets;
	for (const TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
	{
		if (Pair.Value->WorkingState == ELoreWorkingState::Modified
			&& OutOfDateFiles.Contains(Pair.Key)
			&& !LoreUtils::IsMergeableText(Pair.Key))
		{
			Assets.Add(Pair.Key);
		}
	}
	if (Assets.Num() == 0)
	{
		return true;
	}
	Assets.Sort();

	EAppReturnType::Type Answer = EAppReturnType::No; // unattended: take theirs, mine is backed up
	if (!FApp::IsUnattended())
	{
		FString List;
		for (const FString& Abs : Assets)
		{
			FString Rel;
			List += TEXT("    ") + (LoreUtils::ToRelative(RepoRoot, Abs, Rel) ? Rel : Abs) + TEXT("\n");
		}
		Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel,
			FText::Format(LOCTEXT("SyncBothChanged",
				"Someone else submitted a newer version of assets that you have also changed:\n\n{0}\n"
				"Assets can't be merged, so pick which version to keep:\n\n"
				"Yes  =  Keep mine. Your version stays. When you submit it, it replaces theirs.\n"
				"No  =  Take theirs. Their version is synced. Yours is backed up in Saved/LoreSourceControl/SyncBackups.\n"
				"Cancel  =  Don't sync.\n\n"
				"(Changed text files like .ini are merged automatically.)"),
				FText::FromString(List)),
			LOCTEXT("SyncBothChangedTitle", "Sync - changed on both sides"));
	}

	if (Answer == EAppReturnType::Cancel)
	{
		return false;
	}
	for (const FString& Abs : Assets)
	{
		(Answer == EAppReturnType::Yes ? Command.ResolveKeepMine : Command.ResolveTakeTheirs).Add(Abs);
	}
	return true;
}

void FLoreSourceControlProvider::ProcessFinishedQueue()
{
	// Commands finish in order, so the done ones are always at the front.
	// Remove before processing: a completion delegate may call Execute again.
	while (CommandQueue.Num() > 0 && CommandQueue[0]->bWorkDone)
	{
		FLoreCommand* Command = CommandQueue[0];
		CommandQueue.RemoveAt(0);
		ProcessCompletedCommand(*Command);
		delete Command;
	}
}

void FLoreSourceControlProvider::Tick()
{
	ProcessFinishedQueue();

	// Every minute: refresh revision numbers (for "Sync" status) and other
	// people's check-outs, so icons update without the user doing anything.
	const double Now = FPlatformTime::Seconds();
	if (bRepoFound && bHasConnectedOnce && !bConnectInFlight && !bRefreshInFlight && CommandQueue.Num() == 0
		&& Now - LastRefreshTime > LoreProviderLocal::BackgroundRefreshSeconds)
	{
		bRefreshInFlight = true;
		LastRefreshTime = Now;
		FLoreCommand* Command = new FLoreCommand(ISourceControlOperation::Create<FUpdateStatus>(), TArray<FString>(), MakeContext(),
			LockStore, FSourceControlOperationComplete(), ELoreCommandKind::BackgroundRefresh);
		CommandQueue.Add(Command);
		GetWorkerPool()->AddQueuedWork(Command);
	}
}

// ---------------------------------------------------------------------------
// Results (game thread)
// ---------------------------------------------------------------------------

void FLoreSourceControlProvider::ApplyResults(FLoreCommand& Command)
{
	if (Command.bHasRepoInfo)
	{
		if (!Command.RepoInfo.RepoId.IsEmpty())
		{
			RepoId = Command.RepoInfo.RepoId;
		}
		if (!Command.RepoInfo.BranchName.IsEmpty())
		{
			BranchName = Command.RepoInfo.BranchName;
		}
		LocalRevision = Command.RepoInfo.LocalRevision;
		RemoteRevision = Command.RepoInfo.RemoteRevision;
		LastRefreshTime = FPlatformTime::Seconds();
	}

	// Whole-repository lock list: update every file we know about.
	if (Command.bHasFullLockMap)
	{
		for (const TPair<FString, FLoreLockInfo>& Pair : Command.FullLockMap)
		{
			GetStateInternal(Pair.Key); // make sure locked files show their icon
		}
		for (TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
		{
			FLoreSourceControlState& State = Pair.Value.Get();
			if (State.WorkingState == ELoreWorkingState::Outside || State.WorkingState == ELoreWorkingState::NotTracked)
			{
				continue;
			}
			if (const FLoreLockInfo* Lock = Command.FullLockMap.Find(Pair.Key))
			{
				State.LockState = Lock->State;
				State.LockOwner = Lock->Owner;
			}
			else
			{
				State.LockState = ELoreLockState::None;
				State.LockOwner.Reset();
			}
		}
	}

	for (const TPair<FString, TArray<FLoreHistoryEntry>>& Pair : Command.Histories)
	{
		TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(Pair.Key);
		State->History.Reset();
		for (const FLoreHistoryEntry& Entry : Pair.Value)
		{
			State->History.Add(MakeShared<FLoreSourceControlRevision, ESPMode::ThreadSafe>(Pair.Key, Entry, Command.Ctx));
		}
	}

	TSet<FString> Reported;
	for (const FLoreFileStatus& Status : Command.Statuses)
	{
		TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(Status.AbsPath);
		State->ApplyStatus(Status);
		if (OutOfDateFiles.Contains(Status.AbsPath))
		{
			State->bIsCurrent = false; // a per-file refresh must not hide "newer version on server"
		}
		Reported.Add(Status.AbsPath);
	}

	// Files changed on the server since our revision (complete list).
	if (Command.bHasOutOfDate)
	{
		OutOfDateFiles.Reset();
		OutOfDateFiles.Append(Command.OutOfDateFiles);
		for (const FString& Path : OutOfDateFiles)
		{
			GetStateInternal(Path); // so the icon and Sync show even before the editor asks
		}
		for (TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
		{
			Pair.Value->bIsCurrent = !OutOfDateFiles.Contains(Pair.Key);
		}
	}

	// A full scan of a folder lists every changed file in it. Anything we
	// still think is changed but wasn't listed has been submitted/reverted.
	for (const FString& Dir : Command.ScannedDirs)
	{
		const FString Prefix = Dir + TEXT("/");
		for (TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
		{
			if (Reported.Contains(Pair.Key) || !Pair.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				continue;
			}
			FLoreSourceControlState& State = Pair.Value.Get();
			if (State.IsModified())
			{
				State.WorkingState = FPaths::FileExists(Pair.Key) ? ELoreWorkingState::Unchanged : ELoreWorkingState::NotTracked;
				State.bStaged = false;
				State.TimeStamp = FDateTime::Now();
			}
		}
	}

	OnSourceControlStateChanged.Broadcast();
}

void FLoreSourceControlProvider::ReportMessages(const FLoreCommand& Command) const
{
	const bool bBackground = Command.Kind == ELoreCommandKind::BackgroundRefresh;
	if (bBackground && Command.WarningMessages.Num() == 0)
	{
		return; // background errors (e.g. server briefly down) are just logged by LoreUtils
	}

	FMessageLog Log(TEXT("SourceControl"));
	for (const FString& Message : Command.InfoMessages)
	{
		Log.Info(FText::FromString(Message));
	}
	for (const FString& Message : Command.WarningMessages)
	{
		Log.Warning(FText::FromString(Message));
	}
	if (!bBackground)
	{
		for (const FString& Message : Command.ErrorMessages)
		{
			Log.Error(FText::FromString(Message));
		}
	}

	// Pop-up only for things the user actively did. UpdateStatus runs all the
	// time and Connect errors are shown by the login window itself.
	const bool bUserAction = !bBackground && Command.OperationName != LoreOpNames::UpdateStatus && Command.OperationName != LoreOpNames::Connect;
	if (bUserAction && Command.ErrorMessages.Num() > 0)
	{
		Log.Notify(FText::Format(LOCTEXT("OpFailed", "Lore: {0} failed. See the Revision Control log for details."),
			FText::FromName(Command.OperationName)), EMessageSeverity::Error, true);
	}
	else if (Command.WarningMessages.Num() > 0)
	{
		Log.Notify(Command.WarningMessages.Num() == 1
			? FText::FromString(Command.WarningMessages[0])
			: LOCTEXT("OpWarning", "Lore: see the Revision Control log."), EMessageSeverity::Warning, true);
	}
}

FLoreCommand* FLoreSourceControlProvider::FindCommand(const FSourceControlOperationRef& InOperation) const
{
	if (SynchronousCommand && &SynchronousCommand->Operation.Get() == &InOperation.Get())
	{
		return SynchronousCommand;
	}
	for (FLoreCommand* Command : CommandQueue)
	{
		if (&Command->Operation.Get() == &InOperation.Get())
		{
			return Command;
		}
	}
	return nullptr;
}

bool FLoreSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	const FLoreCommand* Command = FindCommand(InOperation);
	return Command && Command->IsCancellable();
}

void FLoreSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
	FLoreCommand* Command = FindCommand(InOperation);
	if (Command && Command->IsCancellable())
	{
		Command->bCancelRequested = true;
	}
}

ECommandResult::Type FLoreSourceControlProvider::ProcessCompletedCommand(FLoreCommand& Command)
{
	// Cancelled: replace the error noise with one clear message.
	const bool bWasCancelled = Command.bCancelRequested && !Command.bCommandSuccessful;
	if (bWasCancelled)
	{
		Command.ErrorMessages.Reset();
		if (Command.OperationName == LoreOpNames::Sync)
		{
			Command.WarningMessages.Add(TEXT("Sync was cancelled. Some files may already be updated; run Sync again to finish."));
		}
		else if (Command.OperationName == LoreOpNames::CheckIn)
		{
			Command.WarningMessages.Add(TEXT("Submit was cancelled before anything was committed. Your files and check-outs are unchanged."));
		}
		else
		{
			Command.InfoMessages.Add(FString::Printf(TEXT("%s was cancelled."), *Command.OperationName.ToString()));
		}
	}

	ApplyResults(Command);

	const FSourceControlOperationRef& Operation = Command.Operation;
	for (const FString& Message : Command.InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(Message));
	}
	for (const FString& Message : Command.WarningMessages)
	{
		Operation->AddInfoMessge(FText::FromString(Message));
	}
	for (const FString& Message : Command.ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(Message));
	}

	if (Command.Kind == ELoreCommandKind::BackgroundRefresh)
	{
		bRefreshInFlight = false;
		const bool bWasAvailable = bAvailable;
		bAvailable = Command.bHasFullLockMap; // server answered the lock query
		if (bWasAvailable && !bAvailable)
		{
			FMessageLog(TEXT("SourceControl")).Warning(LOCTEXT("LostServer", "Lost connection to the Lore server. Check-out and submit are unavailable until it is back."));
		}
	}
	else if (Command.OperationName == LoreOpNames::Connect)
	{
		bConnectInFlight = false;
		bHasConnectedOnce = true;
		bAvailable = Command.bCommandSuccessful;
		if (!Command.bCommandSuccessful)
		{
			StaticCastSharedRef<FConnect>(Operation)->SetErrorText(FText::FromString(FString::Join(Command.ErrorMessages, TEXT("\n"))));
		}
		else
		{
			SaveSettings();
		}
	}
	else if (Command.OperationName == LoreOpNames::CheckIn && Command.bCommandSuccessful)
	{
		StaticCastSharedRef<FCheckIn>(Operation)->SetSuccessMessage(FText::FromString(Command.SuccessMessage));
	}

	ReportMessages(Command);

	const ECommandResult::Type Result = Command.bCommandSuccessful ? ECommandResult::Succeeded
		: (bWasCancelled ? ECommandResult::Cancelled : ECommandResult::Failed);
	Command.OperationCompleteDelegate.ExecuteIfBound(Operation, Result);
	return Result;
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

TArray<TSharedRef<ISourceControlLabel>> FLoreSourceControlProvider::GetLabels(const FString& InMatchingSpec) const
{
	return TArray<TSharedRef<ISourceControlLabel>>();
}

TArray<FSourceControlChangelistRef> FLoreSourceControlProvider::GetChangelists(EStateCacheUsage::Type InStateCacheUsage)
{
	return TArray<FSourceControlChangelistRef>();
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToSync() const
{
	if (!LocalRevision.IsSet() || !RemoteRevision.IsSet())
	{
		return TOptional<bool>();
	}
	return RemoteRevision.GetValue() > LocalRevision.GetValue();
}

TOptional<bool> FLoreSourceControlProvider::HasChangesToCheckIn() const
{
	for (const TPair<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
	{
		if (Pair.Value->CanCheckIn())
		{
			return true;
		}
	}
	return false;
}

TSharedRef<SWidget> FLoreSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SLoreSettings);
}

#undef LOCTEXT_NAMESPACE
