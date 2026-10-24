#include "LoreSourceControlModule.h"

#include "SLoreRestoreRevision.h"

#include "ContentBrowserMenuContexts.h"
#include "Features/IModularFeatures.h"
#include "ISourceControlModule.h"
#include "Modules/ModuleManager.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

void FLoreSourceControlModule::StartupModule()
{
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &LoreProvider);
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLoreSourceControlModule::RegisterMenus));
	UE_LOG(LogLoreSC, Log, TEXT("Lore revision control provider registered."));
}

void FLoreSourceControlModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	LoreProvider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &LoreProvider);
}

void FLoreSourceControlModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Right-click an asset in the Content Browser -> "Lore" section.
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
	FToolMenuSection& Section = Menu->AddSection("LoreSourceControl", LOCTEXT("LoreSection", "Lore"));
	Section.AddDynamicEntry("LoreRestoreRevision", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
	{
		const UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
		if (!Context || Context->SelectedAssets.Num() != 1)
		{
			return;
		}
		ISourceControlModule& RevisionControl = ISourceControlModule::Get();
		if (!RevisionControl.IsEnabled() || RevisionControl.GetProvider().GetName() != FName(TEXT("Lore")))
		{
			return;
		}
		const FString PackageName = Context->SelectedAssets[0].PackageName.ToString();
		InSection.AddMenuEntry(
			"LoreRestoreRevision",
			LOCTEXT("RestoreRevision", "Restore Revision..."),
			LOCTEXT("RestoreRevisionTip", "Bring back an older revision of this asset as a local change (checks it out). Submit to keep it, Revert to undo."),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.History"),
			FUIAction(FExecuteAction::CreateLambda([PackageName]()
			{
				SLoreRestoreRevision::OpenFor(PackageName);
			})));
	}));
}

FLoreSourceControlModule& FLoreSourceControlModule::Get()
{
	return FModuleManager::GetModuleChecked<FLoreSourceControlModule>("LoreSourceControl");
}

bool FLoreSourceControlModule::IsLoaded()
{
	return FModuleManager::Get().IsModuleLoaded("LoreSourceControl");
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl)
