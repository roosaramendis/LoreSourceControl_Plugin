#include "LoreSourceControlRevision.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FLoreSourceControlRevision::FLoreSourceControlRevision(const FString& InFilename, const FLoreHistoryEntry& InEntry, const FLoreContext& InContext)
	: Filename(InFilename)
	, Entry(InEntry)
	, Ctx(InContext)
	, RevisionText(FString::FromInt(InEntry.Number))
	, UserName(InEntry.Creator.IsEmpty() ? InEntry.Committer : InEntry.Creator)
{
	Ctx.CancelFlag = nullptr; // the command this came from is deleted soon
	switch (InEntry.Action)
	{
	case TEXT('A'): Action = TEXT("add"); break;
	case TEXT('D'): Action = TEXT("delete"); break;
	case TEXT('R'): Action = TEXT("move/add"); break;
	default:        Action = TEXT("edit"); break;
	}
}

bool FLoreSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (InOutFilename.IsEmpty())
	{
		// Saved/Diff/<Name>-rev5-<hash>.uasset. The hash part comes from the
		// content address, so the same name always means the same content and
		// an existing file can be reused.
		const FString ShortHash = Entry.Address.Left(8);
		InOutFilename = FPaths::ConvertRelativePathToFull(FPaths::DiffDir() /
			FString::Printf(TEXT("%s-rev%d-%s%s"), *FPaths::GetBaseFilename(Filename), Entry.Number, *ShortHash, *FPaths::GetExtension(Filename, true)));
		if (!ShortHash.IsEmpty() && FPaths::FileExists(InOutFilename))
		{
			return true;
		}
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InOutFilename), true);
	IFileManager::Get().Delete(*InOutFilename, false, true, true);

	TArray<FString> Args = { TEXT("file"), TEXT("write") };
	if (!Entry.Address.IsEmpty())
	{
		Args.Append({ TEXT("--address"), Entry.Address });
	}
	else
	{
		FString Rel;
		if (!LoreUtils::ToRelative(Ctx.RepoRoot, Filename, Rel) || Entry.Signature.IsEmpty())
		{
			return false;
		}
		Args.Append({ TEXT("--path"), Rel, TEXT("--revision"), Entry.Signature });
	}
	Args.Append({ TEXT("--output"), InOutFilename });

	const FLoreResult Result = LoreUtils::Run(Ctx, Args, 600.0);
	if (!Result.Ok() || !FPaths::FileExists(InOutFilename))
	{
		UE_LOG(LogLoreSC, Warning, TEXT("Could not get revision %d of %s: %s"), Entry.Number, *Filename, *Result.Describe());
		return false;
	}
	return true;
}
