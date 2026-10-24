#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "LoreSourceControlProvider.h"

class FLoreSourceControlModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FLoreSourceControlProvider& GetProvider() { return LoreProvider; }

	static FLoreSourceControlModule& Get();
	static bool IsLoaded();

private:
	void RegisterMenus();

	FLoreSourceControlProvider LoreProvider;
};
