// system includes
#include <iostream>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

// library includes
#include <boost/filesystem.hpp>
#include <linuxdeploy/core/appdir.h>
#include <linuxdeploy/core/elf.h>
#include <linuxdeploy/core/log.h>
#include <linuxdeploy/util/util.h>

// local includes
#include "qt-modules.h"
#include "util.h"
#include "deployment.h"
#include "deployers/PluginsDeployerFactory.h"

namespace bf = boost::filesystem;

using namespace linuxdeploy::core;
using namespace linuxdeploy::util::misc;
using namespace linuxdeploy::core::log;
using namespace linuxdeploy::plugin::qt;


int main(const int argc, const char *const *const argv) {
    // set up verbose logging if $DEBUG is set
    if (getenv("DEBUG"))
        ldLog::setVerbosity(LD_DEBUG);

    args::ArgumentParser parser("linuxdeploy Qt plugin",
                                "Bundles Qt resources. For use with an existing AppDir, created by linuxdeploy.");

    args::ValueFlag<bf::path> appDirPath(parser, "appdir path", "Path to an existing AppDir", {"appdir"});
    args::ValueFlagList<std::string> extraPlugins(parser, "plugin",
                                                  "Extra Qt plugin to deploy (specified by name, filename or path)",
                                                  {'p', "extra-plugin"});

    args::Flag pluginType(parser, "", "Print plugin type and exit", {"plugin-type"});
    args::Flag pluginApiVersion(parser, "", "Print plugin API version and exit", {"plugin-api-version"});

    try {
        parser.ParseCLI(argc, argv);
    } catch (const args::ParseError &) {
        std::cerr << parser;
        return 1;
    }

    if (pluginType) {
        std::cout << "input" << std::endl;
        return 0;
    }

    if (pluginApiVersion) {
        std::cout << "0" << std::endl;
        return 0;
    }

    if (!appDirPath) {
        ldLog() << LD_ERROR << "--appdir parameter required" << std::endl;
        std::cout << std::endl << parser;
        return 1;
    }

    if (!bf::is_directory(appDirPath.Get())) {
        ldLog() << LD_ERROR << "No such directory:" << appDirPath.Get() << std::endl;
        return 1;
    }

    appdir::AppDir appDir(appDirPath.Get());

    // allow disabling copyright files deployment via environment variable
    if (getenv("DISABLE_COPYRIGHT_FILES_DEPLOYMENT") != nullptr) {
        ldLog() << std::endl << LD_WARNING << "Copyright files deployment disabled" << std::endl;
        appDir.setDisableCopyrightFilesDeployment(true);
    }

    // check which libraries and plugins the binaries and libraries depend on
    std::set<std::string> libraryNames;
    for (const auto &path : appDir.listSharedLibraries()) {
        libraryNames.insert(path.filename().string());
        try {
            for (const auto &dependency : elf::ElfFile(path).traceDynamicDependencies()) {
                libraryNames.insert(dependency.filename().string());
            }
        } catch (const elf::ElfFileParseError &e) {
            ldLog() << LD_DEBUG << "Failed to parse file as ELF file:" << path << std::endl;
        }
    }

    {
        ldLog() << LD_DEBUG << "Libraries to consider: ";
        for (const auto &libraryName : libraryNames)
            ldLog() << " " << libraryName;
        ldLog() << std::endl;
    }

    // check for Qt modules
    std::vector<QtModule> foundQtModules;
    std::vector<QtModule> extraQtModules;

    auto matchesQtModule = [](std::string libraryName, const QtModule &module) {
        // extract filename if argument is path
        if (bf::is_regular_file(libraryName))
            libraryName = bf::path(libraryName).filename().string();

        // adding the trailing dot makes sure e.g., libQt5WebEngineCore won't be matched as webengine and webenginecore
        const auto &libraryPrefix = module.libraryFilePrefix + ".";

        // match plugin filename
        if (strncmp(libraryName.c_str(), libraryPrefix.c_str(), libraryPrefix.size()) == 0) {
            ldLog() << LD_DEBUG << "-> matches library filename, found module:" << module.name << std::endl;
            return true;
        }

        // match plugin name
        if (strcmp(libraryName.c_str(), module.name.c_str()) == 0) {
            ldLog() << LD_DEBUG << "-> matches module name, found module:" << module.name << std::endl;
            return true;
        }

        return false;
    };

    std::copy_if(QtModules.begin(), QtModules.end(), std::back_inserter(foundQtModules),
                 [&matchesQtModule, &libraryNames](const QtModule &module) {
                     return std::find_if(libraryNames.begin(), libraryNames.end(),
                                         [&matchesQtModule, &module](const std::string &libraryName) {
                                             return matchesQtModule(libraryName, module);
                                         }) != libraryNames.end();
                 });

    std::vector<std::string> extraPluginsFromEnv;
    const auto* const extraPluginsFromEnvData = getenv("EXTRA_QT_PLUGINS");
    if (extraPluginsFromEnvData != nullptr)
        extraPluginsFromEnv = linuxdeploy::util::split(std::string(extraPluginsFromEnvData, ';'));

    for (const auto& pluginsList : {static_cast<std::vector<std::string>>(extraPlugins.Get()), extraPluginsFromEnv}) {
        std::copy_if(QtModules.begin(), QtModules.end(), std::back_inserter(extraQtModules),
            [&matchesQtModule, &libraryNames, &pluginsList](const QtModule &module) {
                return std::find_if(pluginsList.begin(), pluginsList.end(),
                    [&matchesQtModule, &module](const std::string &libraryName) {
                        return matchesQtModule(libraryName, module);
                    }) != pluginsList.end();
            }
        );
    }

    {
        std::set<std::string> moduleNames;
        std::for_each(foundQtModules.begin(), foundQtModules.end(), [&moduleNames](const QtModule &module) {
            moduleNames.insert(module.name);
        });
        ldLog() << "Found Qt modules:" << join(moduleNames) << std::endl;
    }

    {
        std::set<std::string> moduleNames;
        std::for_each(extraQtModules.begin(), extraQtModules.end(), [&moduleNames](const QtModule &module) {
            moduleNames.insert(module.name);
        });
        ldLog() << "Extra Qt modules:" << join(moduleNames) << std::endl;
    }

    if (foundQtModules.empty() && extraQtModules.empty()) {
        ldLog() << LD_ERROR << "Could not find Qt modules to deploy" << std::endl;
        return 1;
    }

    auto qmakePath = findQmake();

    if (qmakePath.empty()) {
        ldLog() << LD_ERROR << "Could not find qmake, please install or provide path using $QMAKE" << std::endl;
        return 1;
    }

    if (!bf::exists(qmakePath)) {
        ldLog() << LD_ERROR << "No such file or directory:" << qmakePath << std::endl;
        return 1;
    }

    ldLog() << "Using qmake:" << qmakePath << std::endl;

    auto qmakeVars = queryQmake(qmakePath);

    if (qmakeVars.empty()) {
        ldLog() << LD_ERROR << "Failed to query Qt paths using qmake -query" << std::endl;
        return 1;
    }

    const bf::path qtPluginsPath = qmakeVars["QT_INSTALL_PLUGINS"];
    const bf::path qtLibexecsPath = qmakeVars["QT_INSTALL_LIBEXECS"];
    const bf::path qtDataPath = qmakeVars["QT_INSTALL_DATA"];
    const bf::path qtTranslationsPath = qmakeVars["QT_INSTALL_TRANSLATIONS"];
    const bf::path qtBinsPath = qmakeVars["QT_INSTALL_BINS"];
    const bf::path qtLibsPath = qmakeVars["QT_INSTALL_LIBS"];
    const bf::path qtInstallQmlPath = qmakeVars["QT_INSTALL_QML"];

    ldLog() << std::endl;
    ldLog() << "QT_INSTALL_LIBS:" << qtLibsPath << std::endl;
    std::ostringstream newLibraryPath;
    newLibraryPath << qtLibsPath.string() << ":" << getenv("LD_LIBRARY_PATH");
    setenv("LD_LIBRARY_PATH", newLibraryPath.str().c_str(), true);
    ldLog() << "Prepending QT_INSTALL_LIBS path to $LD_LIBRARY_PATH, new $LD_LIBRARY_PATH:" << newLibraryPath.str()
            << std::endl;

    std::ostringstream newPath;
    newPath << qtBinsPath.string() << ":" << getenv("PATH");
    setenv("PATH", newPath.str().c_str(), true);
    ldLog() << "Prepending QT_INSTALL_BINS path to $PATH, new $PATH:" << newPath.str() << std::endl;


    auto qtModulesToDeploy = foundQtModules;
    qtModulesToDeploy.reserve(extraQtModules.size());
    std::copy(extraQtModules.begin(), extraQtModules.end(), std::back_inserter(qtModulesToDeploy));

    PluginsDeployerFactory deployerFactory(
        appDir,
        qtPluginsPath,
        qtLibexecsPath,
        qtInstallQmlPath,
        qtTranslationsPath,
        qtDataPath
    );

    for (const auto& module : qtModulesToDeploy) {
        ldLog() << std::endl << "-- Deploying module:" << module.name << "--" << std::endl;

        auto deployers = deployerFactory.getDeployers(module.name);

        for (const auto& deployer : deployers)
            if (!deployer->deploy())
                return 1;
    }

    ldLog() << std::endl << "-- Deploying translations --" << std::endl;
    if (!deployTranslations(appDir, qtTranslationsPath, qtModulesToDeploy)) {
        ldLog() << LD_ERROR << "Failed to deploy translations" << std::endl;
        return 1;
    }

    ldLog() << std::endl << "-- Executing deferred operations --" << std::endl;
    if (!appDir.executeDeferredOperations()) {
        ldLog() << LD_ERROR << "Failed to execute deferred operations" << std::endl;
        return 1;
    }

    ldLog() << std::endl << "-- Creating qt.conf in AppDir --" << std::endl;
    if (!createQtConf(appDir)) {
        ldLog() << LD_ERROR << "Failed to create qt.conf in AppDir" << std::endl;
        return 1;
    }

    ldLog() << std::endl << "-- Creating AppRun hook --" << std::endl;
    if (!createAppRunHook(appDir)) {
        ldLog() << LD_ERROR << "Failed to create AppRun hook in AppDir" << std::endl;
        return 1;
    }

    ldLog() << std::endl << "Done!" << std::endl;
    return 0;
}
