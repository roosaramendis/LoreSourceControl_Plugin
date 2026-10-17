#include "SLoreSettings.h"

#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"

#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SLoreSettings"

namespace LoreSettingsLocal
{
	static FLoreSourceControlProvider& Provider()
	{
		return FLoreSourceControlModule::Get().GetProvider();
	}

	static const FLinearColor Green(0.3f, 0.9f, 0.3f);
	static const FLinearColor Yellow(1.0f, 0.8f, 0.2f);
	static const FLinearColor Red(1.0f, 0.35f, 0.3f);

	// Default ignore list for an Unreal project, written only if missing.
	static void EnsureLoreIgnore(const FString& Root)
	{
		const FString Path = Root / TEXT(".loreignore");
		if (FPaths::FileExists(Path))
		{
			return;
		}
		const FString Content =
			TEXT("Binaries/\n")
			TEXT("Intermediate/\n")
			TEXT("Saved/\n")
			TEXT("DerivedDataCache/\n")
			TEXT(".vscode/\n")
			TEXT(".idea/\n")
			TEXT(".vs/\n")
			TEXT("*.log\n")
			TEXT("*.sln\n")
			TEXT("*.code-workspace\n");
		FFileHelper::SaveStringToFile(Content, *Path);
	}
}

void SLoreSettings::Construct(const FArguments& InArgs)
{
	using namespace LoreSettingsLocal;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SLoreSettings::GetRepoInfoText)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeField(LOCTEXT("ServerLabel", "Server URL"),
				LOCTEXT("ServerHint", "lore://192.168.1.10:41337"),
				LOCTEXT("ServerTip", "Address of the Lore server. Once the project has a .lore folder, the remote stored in .lore/config.toml is used instead."),
				&FLoreSettings::ServerUrl)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeField(LOCTEXT("RepoLabel", "Repository"),
				LOCTEXT("RepoHint", "MY_PROJECT"),
				LOCTEXT("RepoTip", "Repository name on the server. Only used by 'Set Up Repository'."),
				&FLoreSettings::RepositoryName)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeField(LOCTEXT("IdentityLabel", "Identity"),
				LOCTEXT("IdentityHint", "you@studio.com"),
				LOCTEXT("IdentityTip", "Your Lore identity (passed as --identity). Leave empty to use the one in .lore/config.toml."),
				&FLoreSettings::Identity)
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeField(LOCTEXT("BinaryLabel", "Lore executable"),
				FText::FromString(LoreUtils::DefaultLoreBinary()),
				LOCTEXT("BinaryTip", "Full path to the lore program. Leave empty to auto-detect (~/.local/bin, /usr/local/bin, /usr/bin, PATH)."),
				&FLoreSettings::LoreBinary)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CheckBtn", "Check Connection"))
				.ToolTipText(LOCTEXT("CheckTip", "Checks the local .lore repository and whether the server has it. Changes nothing."))
				.IsEnabled(this, &SLoreSettings::IsIdle)
				.OnClicked(this, &SLoreSettings::OnCheckConnection)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("SetupBtn", "Set Up Repository"))
				.ToolTipText(LOCTEXT("SetupTip", "Creates the repository on the server from this project and uploads it (asks first). Only works if this project has no .lore folder yet."))
				.IsEnabled(this, &SLoreSettings::IsIdle)
				.OnClicked(this, &SLoreSettings::OnSetUpRepository)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return FText::FromString(StatusMessage); })
			.ColorAndOpacity_Lambda([this]() { return FSlateColor(StatusColor); })
			.AutoWrapText(true)
		]
	];

	if (Provider().IsRepoFound())
	{
		StartProbe(false);
	}
	else
	{
		SetStatus(TEXT("This project has no Lore repository yet. Fill in Server URL, Repository and Identity, then click 'Set Up Repository'."), Yellow);
	}
}

TSharedRef<SWidget> SLoreSettings::MakeField(const FText& Label, const FText& Hint, const FText& Tooltip, FString FLoreSettings::* Field)
{
	using namespace LoreSettingsLocal;

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.3f).VAlign(VAlign_Center).Padding(2.0f)
		[
			SNew(STextBlock).Text(Label).ToolTipText(Tooltip)
		]
		+ SHorizontalBox::Slot().FillWidth(0.7f).Padding(2.0f)
		[
			SNew(SEditableTextBox)
			.HintText(Hint)
			.ToolTipText(Tooltip)
			.IsEnabled(this, &SLoreSettings::IsIdle)
			.Text_Lambda([Field]() { return FText::FromString(Provider().GetSettings().*Field); })
			.OnTextChanged_Lambda([Field](const FText& NewText)
			{
				FLoreSettings Settings = Provider().GetSettings();
				Settings.*Field = NewText.ToString();
				Provider().SetSettings(Settings, false);
			})
			.OnTextCommitted_Lambda([Field](const FText& NewText, ETextCommit::Type)
			{
				FLoreSettings Settings = Provider().GetSettings();
				Settings.*Field = NewText.ToString();
				Provider().SetSettings(Settings, true);
			})
		];
}

FText SLoreSettings::GetRepoInfoText() const
{
	using namespace LoreSettingsLocal;
	const FLoreSourceControlProvider& P = Provider();
	if (P.IsRepoFound())
	{
		return FText::Format(LOCTEXT("RepoFound", "Lore repository: {0}\nServer in use: {1}"),
			FText::FromString(P.GetRepoRoot()), FText::FromString(P.MakeContext().ServerUrl));
	}
	return FText::Format(LOCTEXT("RepoMissing", "No .lore folder found in {0}"), FText::FromString(P.GetRepoRoot()));
}

void SLoreSettings::SetStatus(const FString& Message, const FLinearColor& Color)
{
	StatusMessage = Message;
	StatusColor = Color;
}

FReply SLoreSettings::OnCheckConnection()
{
	StartProbe(false);
	return FReply::Handled();
}

FReply SLoreSettings::OnSetUpRepository()
{
	StartProbe(true);
	return FReply::Handled();
}

void SLoreSettings::StartProbe(bool bForSetup)
{
	using namespace LoreSettingsLocal;

	FLoreSourceControlProvider& P = Provider();
	P.RefreshRepoRoot();
	const FLoreContext Ctx = P.MakeContext();
	const bool bLocalRepo = P.IsRepoFound();

	if (Ctx.ServerUrl.IsEmpty())
	{
		SetStatus(TEXT("Enter the Server URL first (e.g. lore://192.168.1.10:41337)."), Red);
		return;
	}
	if (Ctx.Identity.IsEmpty())
	{
		SetStatus(TEXT("Enter your Identity first (e.g. you@studio.com)."), Red);
		return;
	}

	bBusy = true;
	SetStatus(TEXT("Checking..."), FLinearColor::White);

	TWeakPtr<SLoreSettings> Weak = StaticCastSharedRef<SLoreSettings>(AsShared());
	(void)Async(EAsyncExecution::ThreadPool, [Weak, Ctx, bLocalRepo, bForSetup]()
	{
		FLoreProbeResult Result;
		Result.bLocalRepo = bLocalRepo;
		if (bLocalRepo)
		{
			const FLoreResult Status = LoreUtils::Run(Ctx, { TEXT("status") }, 60.0);
			if (Status.Ok())
			{
				FLoreRepoInfo Info;
				LoreUtils::ParseStatus(Status.Output, Info);
				Result.LocalRepoId = Info.RepoId;
			}
			else
			{
				Result.LocalError = Status.Describe();
			}
		}
		const FLoreResult List = LoreUtils::Run(Ctx, { TEXT("repository"), TEXT("list"), Ctx.ServerUrl }, 30.0);
		Result.bServerOk = List.Ok();
		if (List.Ok())
		{
			LoreUtils::ParseRepositoryList(List.Output, Result.ServerRepos);
		}
		else
		{
			Result.ServerError = List.Describe();
		}

		AsyncTask(ENamedThreads::GameThread, [Weak, Result, Ctx, bForSetup]()
		{
			if (TSharedPtr<SLoreSettings> Pinned = Weak.Pin())
			{
				Pinned->OnProbeDone(Result, Ctx, bForSetup);
			}
		});
	});
}

void SLoreSettings::OnProbeDone(const FLoreProbeResult& Result, const FLoreContext& Ctx, bool bForSetup)
{
	using namespace LoreSettingsLocal;
	bBusy = false;

	if (!Result.bServerOk)
	{
		SetStatus(FString::Printf(TEXT("Cannot reach the server %s.\n%s"), *Ctx.ServerUrl, *Result.ServerError), Red);
		return;
	}

	TArray<FString> Names;
	for (const TPair<FString, FString>& Repo : Result.ServerRepos)
	{
		Names.Add(Repo.Key);
	}
	const FString NameList = Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("(none)"));

	// ---- this project already has a .lore folder ----
	if (Result.bLocalRepo)
	{
		if (!Result.LocalError.IsEmpty() || Result.LocalRepoId.IsEmpty())
		{
			SetStatus(FString::Printf(TEXT("The .lore folder in %s looks broken: lore status failed.\n%s"), *Ctx.RepoRoot, *Result.LocalError), Red);
			return;
		}
		bool bOnServer = false;
		FString ServerName;
		for (const TPair<FString, FString>& Repo : Result.ServerRepos)
		{
			if (Repo.Value.Equals(Result.LocalRepoId, ESearchCase::IgnoreCase))
			{
				bOnServer = true;
				ServerName = Repo.Key;
			}
		}
		if (bOnServer)
		{
			SetStatus(FString::Printf(TEXT("OK: this project is repository '%s' on %s.%s Click 'Accept Settings'."),
				*ServerName, *Ctx.ServerUrl, bForSetup ? TEXT(" It is already set up.") : TEXT("")), Green);
		}
		else if (Result.ServerRepos.Num() == 0)
		{
			SetStatus(FString::Printf(TEXT("The server answered but listed no repositories for identity '%s'. If the repository exists, this identity may not have access."),
				*Ctx.Identity), Yellow);
		}
		else
		{
			SetStatus(FString::Printf(TEXT("This project's .lore repository (id %s) is NOT on %s.\nRepositories on that server: %s\n"
				"Check the server address in .lore/config.toml (remote_url), or move the .lore folder away and use 'Set Up Repository' to start fresh."),
				*Result.LocalRepoId, *Ctx.ServerUrl, *NameList), Red);
		}
		return;
	}

	// ---- no .lore folder yet ----
	if (Ctx.RepositoryName.IsEmpty())
	{
		SetStatus(TEXT("Enter a Repository name first."), Red);
		return;
	}
	if (Names.Contains(Ctx.RepositoryName))
	{
		SetStatus(FString::Printf(TEXT("The server already has a repository named '%s'. To work on it, clone it into an EMPTY folder, then open the .uproject from there:\n"
			"lore clone %s/%s ~/Projects/%s"),
			*Ctx.RepositoryName, *Ctx.ServerUrl, *Ctx.RepositoryName, *Ctx.RepositoryName), Yellow);
		return;
	}
	if (!bForSetup)
	{
		SetStatus(FString::Printf(TEXT("Server OK. '%s' does not exist there yet. Click 'Set Up Repository' to create it and upload this project."),
			*Ctx.RepositoryName), Yellow);
		return;
	}

	const FText Question = FText::Format(LOCTEXT("ConfirmCreate",
		"Create repository '{0}' on {1} and upload this project?\n\nFolder: {2}\n\n"
		"A .loreignore file is written first (if missing) so Binaries, Intermediate, Saved and DerivedDataCache are not uploaded."),
		FText::FromString(Ctx.RepositoryName), FText::FromString(Ctx.ServerUrl), FText::FromString(Ctx.RepoRoot));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Question, LOCTEXT("ConfirmCreateTitle", "Set Up Lore Repository")) != EAppReturnType::Yes)
	{
		SetStatus(TEXT("Cancelled."), FLinearColor::White);
		return;
	}
	StartCreate(Ctx);
}

void SLoreSettings::StartCreate(const FLoreContext& Ctx)
{
	using namespace LoreSettingsLocal;

	EnsureLoreIgnore(Ctx.RepoRoot);
	bBusy = true;
	SetStatus(TEXT("Creating the repository and uploading the project. This can take a while for big projects..."), FLinearColor::White);

	TWeakPtr<SLoreSettings> Weak = StaticCastSharedRef<SLoreSettings>(AsShared());
	(void)Async(EAsyncExecution::ThreadPool, [Weak, Ctx]()
	{
		bool bOk = true;
		FString Message;
		auto Step = [&Ctx, &bOk, &Message](const TArray<FString>& Args, double Timeout, const TCHAR* What)
		{
			if (!bOk)
			{
				return;
			}
			const FLoreResult Result = LoreUtils::Run(Ctx, Args, Timeout);
			if (!Result.Ok())
			{
				bOk = false;
				Message = FString::Printf(TEXT("%s failed:\n%s"), What, *Result.Describe());
			}
		};

		Step({ TEXT("repository"), TEXT("create"), Ctx.ServerUrl + TEXT("/") + Ctx.RepositoryName }, 120.0, TEXT("Creating the repository"));
		Step({ TEXT("stage"), TEXT("--scan"), TEXT(".") }, 3600.0, TEXT("Adding the project files"));
		Step({ TEXT("commit"), TEXT("Initial commit") }, 3600.0, TEXT("The initial commit"));
		Step({ TEXT("push") }, 3600.0, TEXT("Uploading to the server"));

		AsyncTask(ENamedThreads::GameThread, [Weak, bOk, Message]()
		{
			if (TSharedPtr<SLoreSettings> Pinned = Weak.Pin())
			{
				Pinned->OnCreateDone(bOk, Message);
			}
		});
	});
}

void SLoreSettings::OnCreateDone(bool bOk, const FString& Message)
{
	using namespace LoreSettingsLocal;
	bBusy = false;

	FLoreSourceControlProvider& P = Provider();
	P.RefreshRepoRoot();
	if (bOk)
	{
		SetStatus(TEXT("Done: repository created and project uploaded. Click 'Accept Settings'."), Green);
		P.StartConnectCheck();
	}
	else
	{
		SetStatus(Message + (P.IsRepoFound() ? TEXT("\nThe local .lore folder was created; click 'Check Connection' to see what is on the server.") : TEXT("")), Red);
	}
}

#undef LOCTEXT_NAMESPACE
