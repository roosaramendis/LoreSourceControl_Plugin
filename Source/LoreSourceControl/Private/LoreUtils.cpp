#include "LoreUtils.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_UNIX
#include <signal.h>
#endif

DEFINE_LOG_CATEGORY(LogLoreSC);

namespace LoreUtilsLocal
{
	// Wraps a value in single quotes for bash. Inside single quotes nothing is
	// special except the quote itself, which becomes '\''  -> no injection possible.
	static FString ShellQuote(const FString& In)
	{
		return TEXT("'") + In.Replace(TEXT("'"), TEXT("'\\''")) + TEXT("'");
	}

	static FString StripAnsiAndCR(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR Ch = In[i];
			if (Ch == TCHAR(27) && i + 1 < In.Len() && In[i + 1] == TEXT('['))
			{
				i += 2;
				while (i < In.Len() && !FChar::IsAlpha(In[i]))
				{
					++i;
				}
				continue;
			}
			if (Ch != TEXT('\r'))
			{
				Out.AppendChar(Ch);
			}
		}
		return Out;
	}

	static TOptional<int32> ParseNumberAfter(const FString& Line, const TCHAR* Marker)
	{
		const int32 Index = Line.Find(Marker, ESearchCase::CaseSensitive);
		if (Index == INDEX_NONE)
		{
			return TOptional<int32>();
		}
		const FString Rest = Line.Mid(Index + FCString::Strlen(Marker)).TrimStart();
		FString Digits;
		for (const TCHAR Ch : Rest)
		{
			if (!FChar::IsDigit(Ch))
			{
				break;
			}
			Digits.AppendChar(Ch);
		}
		if (Digits.IsEmpty())
		{
			return TOptional<int32>();
		}
		return FCString::Atoi(*Digits);
	}
}

FString FLoreResult::Describe() const
{
	if (!bLaunched)
	{
		return Output.IsEmpty() ? FString(TEXT("Could not start lore.")) : Output;
	}
	if (bTimedOut)
	{
		return TEXT("The lore command timed out.");
	}
	if (Output.IsEmpty())
	{
		return FString::Printf(TEXT("lore exited with code %d"), ExitCode);
	}
	return Output.Len() > 2000 ? TEXT("...") + Output.Right(2000) : Output;
}

FLoreResult LoreUtils::Run(const FLoreContext& Ctx, const TArray<FString>& Args, double TimeoutSeconds, const FString& WorkingDir)
{
	using namespace LoreUtilsLocal;

	FLoreResult Result;
	const FString Dir = WorkingDir.IsEmpty() ? Ctx.RepoRoot : WorkingDir;
	const FString Binary = Ctx.LoreBinary.IsEmpty() ? FString(TEXT("lore")) : Ctx.LoreBinary;

	// Global options go before the sub-command.
	FString CommandLine = ShellQuote(Binary) + TEXT(" --no-pager --non-interactive");
	if (!Ctx.Identity.IsEmpty())
	{
		CommandLine += TEXT(" --identity ") + ShellQuote(Ctx.Identity);
	}
	for (const FString& Arg : Args)
	{
		CommandLine += TEXT(" ") + ShellQuote(Arg);
	}

	// A tiny script per command: cd into the repo, then `exec` lore so the
	// process id we get back IS lore (lets us kill it on timeout).
	const FString Script = FString::Printf(
		TEXT("#!/bin/bash\nexport NO_COLOR=1\nexport TERM=dumb\nexec 2>&1\ncd %s || exit 97\nexec %s\n"),
		*ShellQuote(Dir), *CommandLine);

	const FString ScriptPath = FPaths::Combine(FString(FPlatformProcess::UserTempDir()),
		FString::Printf(TEXT("lore_sc_%s.sh"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	if (!FFileHelper::SaveStringToFile(Script, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.Output = FString::Printf(TEXT("Could not write temporary script %s"), *ScriptPath);
		return Result;
	}

	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	uint32 ProcessId = 0;
	FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("/bin/bash"), *ScriptPath,
		false, true, true, &ProcessId, 0, nullptr, WritePipe, nullptr);

	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		IFileManager::Get().Delete(*ScriptPath, false, true, true);
		Result.Output = TEXT("Could not start /bin/bash to run lore.");
		return Result;
	}

	Result.bLaunched = true;
	const double StartTime = FPlatformTime::Seconds();
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		Result.Output += FPlatformProcess::ReadPipe(ReadPipe);
		if (TimeoutSeconds > 0.0 && FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			Result.bTimedOut = true;
#if PLATFORM_UNIX
			if (ProcessId != 0)
			{
				kill(static_cast<pid_t>(ProcessId), SIGKILL);
			}
#endif
			FPlatformProcess::TerminateProc(Proc, true);
			break;
		}
		FPlatformProcess::Sleep(0.005f);
	}
	Result.Output += FPlatformProcess::ReadPipe(ReadPipe);

	if (!Result.bTimedOut)
	{
		int32 ReturnCode = -1;
		FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
		Result.ExitCode = ReturnCode;
	}

	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	IFileManager::Get().Delete(*ScriptPath, false, true, true);

	Result.Output = StripAnsiAndCR(Result.Output).TrimStartAndEnd();

	const FString ArgsText = FString::Join(Args, TEXT(" "));
	if (Result.Ok())
	{
		UE_LOG(LogLoreSC, Verbose, TEXT("lore %s -> ok\n%s"), *ArgsText, *Result.Output);
	}
	else
	{
		UE_LOG(LogLoreSC, Log, TEXT("lore %s -> exit %d%s\n%s"), *ArgsText, Result.ExitCode,
			Result.bTimedOut ? TEXT(" (timed out)") : TEXT(""), *Result.Output);
	}
	return Result;
}

FString LoreUtils::NormalizePath(const FString& InPath)
{
	FString Path = FPaths::ConvertRelativePathToFull(InPath);
	FPaths::NormalizeFilename(Path);
	FPaths::CollapseRelativeDirectories(Path);
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

bool LoreUtils::ToRelative(const FString& RepoRoot, const FString& AbsPath, FString& OutRelPath)
{
	const FString Prefix = RepoRoot + TEXT("/");
	if (RepoRoot.IsEmpty() || !AbsPath.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	OutRelPath = AbsPath.Mid(Prefix.Len());
	return !OutRelPath.IsEmpty();
}

FString LoreUtils::ToAbsolute(const FString& RepoRoot, const FString& RelPath)
{
	return NormalizePath(RepoRoot / RelPath);
}

FString LoreUtils::FindRepoRoot(const FString& StartDir)
{
	FString Dir = NormalizePath(StartDir);
	while (!Dir.IsEmpty())
	{
		if (FPaths::DirectoryExists(Dir / TEXT(".lore")))
		{
			return Dir;
		}
		const FString Parent = FPaths::GetPath(Dir);
		if (Parent.IsEmpty() || Parent == Dir)
		{
			break;
		}
		Dir = Parent;
	}
	return FString();
}

FString LoreUtils::DefaultLoreBinary()
{
	// The editor is often started from a desktop launcher whose PATH does not
	// contain ~/.local/bin, so look in the usual install locations first.
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!Home.IsEmpty())
	{
		const FString LocalBin = Home / TEXT(".local/bin/lore");
		if (FPaths::FileExists(LocalBin))
		{
			return LocalBin;
		}
	}
	if (FPaths::FileExists(TEXT("/usr/local/bin/lore")))
	{
		return TEXT("/usr/local/bin/lore");
	}
	if (FPaths::FileExists(TEXT("/usr/bin/lore")))
	{
		return TEXT("/usr/bin/lore");
	}
	return TEXT("lore");
}

FString LoreUtils::ReadConfigValue(const FString& RepoRoot, const FString& Key)
{
	TArray<FString> Lines;
	if (RepoRoot.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *(RepoRoot / TEXT(".lore/config.toml"))))
	{
		return FString();
	}
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.StartsWith(TEXT("[")))
		{
			break; // only top-level keys
		}
		FString Left, Right;
		if (Line.Split(TEXT("="), &Left, &Right) && Left.TrimStartAndEnd() == Key)
		{
			FString Value = Right.TrimStartAndEnd();
			Value.RemoveFromStart(TEXT("\""));
			Value.RemoveFromEnd(TEXT("\""));
			return Value;
		}
	}
	return FString();
}

void LoreUtils::ParseStatus(const FString& Output, FLoreRepoInfo& OutInfo)
{
	using namespace LoreUtilsLocal;

	enum class ESection : uint8 { None, Staged, Unstaged };
	ESection Section = ESection::None;

	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, false);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.IsEmpty())
		{
			continue;
		}
		if (Line.StartsWith(TEXT("Repository ")))
		{
			FString Id = Line.Mid(11).TrimStartAndEnd();
			int32 Space = INDEX_NONE;
			if (Id.FindChar(TEXT(' '), Space))
			{
				Id.LeftInline(Space);
			}
			OutInfo.RepoId = Id;
			continue;
		}
		if (Line.StartsWith(TEXT("On branch ")))
		{
			OutInfo.LocalRevision = ParseNumberAfter(Line, TEXT(" revision "));
			FString Branch = Line.Mid(10);
			const int32 RevIndex = Branch.Find(TEXT(" revision "), ESearchCase::CaseSensitive);
			if (RevIndex != INDEX_NONE)
			{
				Branch.LeftInline(RevIndex);
			}
			OutInfo.BranchName = Branch.TrimStartAndEnd();
			continue;
		}
		if (Line.StartsWith(TEXT("Remote revision ")))
		{
			OutInfo.RemoteRevision = ParseNumberAfter(Line, TEXT("Remote revision "));
			continue;
		}
		if (Line.StartsWith(TEXT("Remote branch does not exist")))
		{
			OutInfo.RemoteRevision = 0;
			continue;
		}
		if (Line.EndsWith(TEXT(":")))
		{
			if (Line.Contains(TEXT("not staged")))
			{
				Section = ESection::Unstaged;
			}
			else if (Line.Contains(TEXT("staged")))
			{
				Section = ESection::Staged;
			}
			else if (Line.Contains(TEXT("ntracked")))
			{
				Section = ESection::Unstaged;
			}
			else
			{
				Section = ESection::None;
			}
			continue;
		}
		if (Section == ESection::None)
		{
			continue;
		}
		if (Line.Len() >= 3 && Line[1] == TEXT(' ') && (FChar::IsUpper(Line[0]) || Line[0] == TEXT('?')))
		{
			FLoreStatusEntry Entry;
			Entry.Code = Line[0];
			Entry.bStaged = Section == ESection::Staged;
			Entry.RelPath = Line.Mid(2).TrimStartAndEnd();
			const int32 Arrow = Entry.RelPath.Find(TEXT(" -> "), ESearchCase::CaseSensitive);
			if (Arrow != INDEX_NONE)
			{
				Entry.RelPath = Entry.RelPath.Mid(Arrow + 4).TrimStartAndEnd(); // moves: keep the new path
			}
			Entry.RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Entry.RelPath.EndsWith(TEXT("/")))
			{
				Entry.RelPath.LeftChopInline(1);
			}
			if (!Entry.RelPath.IsEmpty())
			{
				OutInfo.Entries.Add(Entry);
			}
		}
	}
}

void LoreUtils::ParseLocks(const FString& Output, TMap<FString, FString>& OutLocks)
{
	// Formats seen with lore 0.8.4:
	//   lock status: "Content/x.uasset by <unknown> on Fri, 16 Oct 2026 15:46:14 +0000"
	//   lock query : "Content/x.uasset by <unknown> on branch e7263..."
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, true);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		const int32 ByIndex = Line.Find(TEXT(" by "), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (ByIndex <= 0)
		{
			continue;
		}
		FString Path = Line.Left(ByIndex).TrimStartAndEnd();
		FString Owner = Line.Mid(ByIndex + 4);
		const int32 OnIndex = Owner.Find(TEXT(" on "), ESearchCase::CaseSensitive);
		if (OnIndex != INDEX_NONE)
		{
			Owner.LeftInline(OnIndex);
		}
		Owner.TrimStartAndEndInline();
		if (Owner == TEXT("<unknown>"))
		{
			Owner.Reset();
		}
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!Path.IsEmpty())
		{
			OutLocks.Add(Path, Owner);
		}
	}
}

void LoreUtils::ParseRepositoryList(const FString& Output, TArray<TPair<FString, FString>>& OutRepos)
{
	// "MY_PROJECT (01a14562a9b573029bf82f2a0156262a)"
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, true);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		int32 Open = INDEX_NONE;
		if (!Line.EndsWith(TEXT(")")) || !Line.FindLastChar(TEXT('('), Open) || Open <= 0)
		{
			continue;
		}
		const FString Name = Line.Left(Open).TrimStartAndEnd();
		const FString Id = Line.Mid(Open + 1, Line.Len() - Open - 2).TrimStartAndEnd();
		if (!Name.IsEmpty() && !Id.IsEmpty())
		{
			OutRepos.Add(TPair<FString, FString>(Name, Id));
		}
	}
}

TOptional<int32> LoreUtils::ParseFirstHistoryRevision(const FString& Output)
{
	// `lore file history <path> 1 --oneline` -> "3 locktest edit"
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, true);
	for (const FString& Raw : Lines)
	{
		FString First, Rest;
		const FString Line = Raw.TrimStartAndEnd();
		if (!Line.Split(TEXT(" "), &First, &Rest))
		{
			First = Line;
		}
		if (!First.IsEmpty() && First.IsNumeric())
		{
			return FCString::Atoi(*First);
		}
	}
	return TOptional<int32>();
}

TOptional<int32> LoreUtils::ParseCommittedRevision(const FString& Output)
{
	// "Revision  : 2"
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, true);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		FString Left, Right;
		if (Line.StartsWith(TEXT("Revision")) && Line.Split(TEXT(":"), &Left, &Right))
		{
			Right.TrimStartAndEndInline();
			if (Right.IsNumeric())
			{
				return FCString::Atoi(*Right);
			}
		}
	}
	return TOptional<int32>();
}

// ---------------------------------------------------------------------------
// FLoreLockStore
// ---------------------------------------------------------------------------

void FLoreLockStore::SetFile(const FString& InFilePath)
{
	FScopeLock Lock(&Mutex);
	FilePath = InFilePath;
	Locks.Reset();

	TArray<FString> Lines;
	if (FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		for (const FString& Line : Lines)
		{
			FString Rel, Owner;
			if (!Line.Split(TEXT("\t"), &Rel, &Owner))
			{
				Rel = Line;
			}
			Rel.TrimStartAndEndInline();
			if (!Rel.IsEmpty())
			{
				Locks.Add(Rel, Owner.TrimStartAndEnd());
			}
		}
	}
}

bool FLoreLockStore::Find(const FString& RelPath, FString& OutOwner) const
{
	FScopeLock Lock(&Mutex);
	if (const FString* Owner = Locks.Find(RelPath))
	{
		OutOwner = *Owner;
		return true;
	}
	return false;
}

void FLoreLockStore::Add(const FString& RelPath, const FString& Owner)
{
	FScopeLock Lock(&Mutex);
	Locks.Add(RelPath, Owner);
	SaveLocked();
}

void FLoreLockStore::Remove(const FString& RelPath)
{
	FScopeLock Lock(&Mutex);
	if (Locks.Remove(RelPath) > 0)
	{
		SaveLocked();
	}
}

TArray<FString> FLoreLockStore::GetAll() const
{
	FScopeLock Lock(&Mutex);
	TArray<FString> Keys;
	Locks.GetKeys(Keys);
	return Keys;
}

void FLoreLockStore::SaveLocked() const
{
	if (FilePath.IsEmpty())
	{
		return;
	}
	FString Text;
	for (const TPair<FString, FString>& Pair : Locks)
	{
		Text += Pair.Key + TEXT("\t") + Pair.Value + TEXT("\n");
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	FFileHelper::SaveStringToFile(Text, *FilePath);
}
