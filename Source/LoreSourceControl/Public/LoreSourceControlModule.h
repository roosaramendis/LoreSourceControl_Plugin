#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "LoreSourceControlProvider.h"

class FLoreSourceControlModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    FLoreSourceControlProvider& GetProvider();

private:
    FLoreSourceControlProvider LoreProvider;
};