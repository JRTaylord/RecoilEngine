/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/LogOutput.h"

#include "System/StringUtil.h"
#include "Game/GameVersion.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/FileSystem.h"
#include "System/Log/DefaultFilter.h"
#include "System/Log/FileSink.h"
#include "System/Log/ILog.h"
#include "System/Log/Level.h"
#include "System/Log/LogUtil.h"
#include "System/Platform/Misc.h"
#include "System/Platform/Threading.h"
#include "System/UnorderedMap.hpp"

#include <string>
#include <iostream>
#include <sstream>
#include <ranges>

#include <cassert>
#include <cstring>


/******************************************************************************/
/******************************************************************************/

CONFIG(bool, RotateLogFiles)
	.defaultValue(false)
	.description("Rotate logfiles, old logfiles will be moved into the subfolder \"log\".");

CONFIG(std::string, LogSections)
	.defaultValue("")
	.description("Comma-separated list of enabled logsections, see infolog.txt / console output for possible values.");


CONFIG(int, LogFlushLevel)
	.defaultValue(LOG_LEVEL_ERROR)
	.description("Flush the logfile when a message's level exceeds this value. ERROR is flushed by default, WARNING is not.");

CONFIG(int, LogRepeatLimit)
	.defaultValue(0)
	.description("Allow at most this many consecutive identical messages to be logged. Set to 0 to disable the limit.");

/******************************************************************************/
/******************************************************************************/

static spring::unordered_map<std::string, int> GetEnabledSections() {
	spring::unordered_map<std::string, int> sectionLevelMap;

	std::string enabledSections = "";

#if defined(UNITSYNC)
	#if defined(DEBUG)
	// unitsync logging in debug mode always on
	// configHandler cannot be accessed here in unitsync, as it may not exist.
	enabledSections += "unitsync,ArchiveScanner,";
	#endif
#else
	#if defined(DEDICATED)
	enabledSections += "DedicatedServer,";
	#endif
	#if !defined(DEBUG)
	// Always show at least INFO level of these sections
	enabledSections += "Sound:35,VFS:30,";
	#endif
	enabledSections += StringToLower(configHandler->GetString("LogSections"));
	enabledSections += ",";
#endif

	if (auto envVar = getenv("SPRING_LOG_SECTIONS"); envVar != nullptr) {
		// allow disabling all sections from the env var by setting it to "none"
		std::string envSections = StringToLower(envVar);
		if (envSections == "none") {
			enabledSections = "";
		} else {
			enabledSections += envSections;
		}
	}

	enabledSections = StringToLower(enabledSections);
	enabledSections = StringStrip(enabledSections, " \t\n\r");

	for (const auto& subView: enabledSections | std::views::split(',')) {
		auto sub = std::string_view(subView.begin(), subView.end());
		if (sub.empty())
			continue;

		std::string logSec, logLvl;
		if (const size_t sepChr = sub.find(':'); sepChr != std::string_view::npos) {
			logSec = sub.substr(0, sepChr);
			logLvl = sub.substr(sepChr + 1, std::string_view::npos);
		} else {
			logSec = sub;
			logLvl = "";
		}

		if (!logLvl.empty()) {
			sectionLevelMap[logSec] = StringToInt(logLvl);
		} else {
			#if defined(DEBUG)
			sectionLevelMap[logSec] = LOG_LEVEL_DEBUG;
			#else
			sectionLevelMap[logSec] = DEFAULT_LOG_LEVEL;
			#endif
		}
	}

	return sectionLevelMap;
}


CLogOutput logOutput;

CLogOutput::CLogOutput()
	: fileName("")
	, filePath("")
{
	// multiple infologs can't exist together!
	assert(this == &logOutput);

	SetFileName("infolog.txt");
}


void CLogOutput::SetFileName(std::string fname)
{
	assert(!IsInitialized());
	fileName = fname;
}

std::string CLogOutput::CreateFilePath(const std::string& fileName)
{
	return (FileSystem::EnsurePathSepAtEnd(FileSystem::GetCwd()) + fileName);
}


void CLogOutput::RotateLogFile() const
{
	if (!FileSystem::FileExists(filePath))
		return;

	// logArchiveDir: /absolute/writeable/data/dir/log/
	const std::string logArchiveDir = filePath.substr(0, filePath.find_last_of("/\\") + 1) + "log" + FileSystem::GetNativePathSeparator();
	const std::string archivedLogFile = logArchiveDir + FileSystem::GetFileModificationDate(filePath) + "_" + fileName;

	// create the log archive dir if it does not exist yet
	if (!FileSystem::DirExists(logArchiveDir))
		FileSystem::CreateDirectory(logArchiveDir);

	// move the old log to the archive dir
	if (rename(filePath.c_str(), archivedLogFile.c_str()) != 0) {
		// no log here yet
		std::cerr << "Failed rotating the log file" << std::endl;
	}
}



void CLogOutput::Initialize()
{
	assert(configHandler != nullptr);

	if (IsInitialized())
		return;

	filePath = CreateFilePath(fileName);

	if (configHandler->GetBool("RotateLogFiles"))
		RotateLogFile();

	log_filter_setRepeatLimit(configHandler->GetInt("LogRepeatLimit")); // all sinks
	log_file_addLogFile(filePath.c_str(), nullptr, LOG_LEVEL_ALL, configHandler->GetInt("LogFlushLevel"));

	LOG("LogOutput initialized. Logging to %s", filePath.c_str());
}


/**
 * @brief initialize the log sections
 *
 * This writes a list of all available and all enabled sections to the log.
 *
 * Log sections can be enabled using the configuration key "LogSections",
 * or the environment variable "SPRING_LOG_SECTIONS".
 *
 * Both specify a comma-separated list of sections that should be enabled.
 * The lists from both sources are combined, there is no overriding.
 *
 * A section that is enabled by default, can not be disabled.
 */
void CLogOutput::LogSectionInfo()
{
	// the new systems (ILog.h) log-sub-systems are called sections
	const char** registeredSections = log_filter_section_getRegisteredSet();

	// enabled sections is a superset of the ones specified in the
	// environment and the ones specified in the configuration file.
	const auto& enabledSections = GetEnabledSections();

	std::stringstream availableLogSectionsStr;
	std::stringstream   enabledLogSectionsStr;

	for (int i = 0, n = log_filter_section_getNumRegisteredSections(); i < n; i++) {
		const char* regSec = registeredSections[i];

		availableLogSectionsStr << "\n    ";
		availableLogSectionsStr << "[A] " << regSec;

		// enabled sections (keys) are in lower-case
		const auto sectionIter = enabledSections.find(StringToLower(regSec));

		// skip if section is registered but not enabled
		if (sectionIter == enabledSections.end())
			continue;

		// user-specified wanted level for this section
		const int sectionLevel = sectionIter->second;

		if (sectionLevel >= LOG_LEVEL_NONE)
			continue;

		// find the nearest lower known log-level (in descending order)
		const int logLevel = log_util_getNearestLevel(sectionLevel);

		// levels can't go lower than this
		if (logLevel < 0)
			continue;

		log_filter_section_setMinLevel(logLevel, regSec);

		enabledLogSectionsStr << "\n    ";
		enabledLogSectionsStr << "[E] " << regSec << " (" << log_util_levelToString(logLevel) << ")";
	}

	LOG("============== <Log Sections ([A]vailable, [E]nabled)> ==============");
	LOG("  %s%s", (availableLogSectionsStr.str()).c_str(), (enabledLogSectionsStr.str()).c_str());
	LOG("  ");
	LOG("  Enable or disable log sections using the LogSections configuration key");
	LOG("  or the SPRING_LOG_SECTIONS environment variable (both comma separated).");
	LOG("  Use \"none\" to disable the default log sections.");
	LOG("============== </Log Sections> ==============\n");
}

void CLogOutput::LogConfigInfo()
{
	LOG("============== <User Config> ==============");

	// list user's non-default config; exclude non-engine tags
	for (const auto& it: configHandler->GetDataWithoutDefaults()) {
		if (ConfigVariable::GetMetaData(it.first) == nullptr)
			continue;

		LOG("  %s = %s", it.first.c_str(), it.second.c_str());
	}

	LOG("============== </User Config> ==============\n");

}

void CLogOutput::LogSystemInfo()
{
	LOG("============== <User System> ==============");
	LOG("  Spring Engine Version: %s", SpringVersion::GetFull().c_str());
	LOG("      Build Environment: %s", SpringVersion::GetBuildEnvironment().c_str());
	LOG("       Compiler Version: %s", SpringVersion::GetCompiler().c_str());
	LOG("       Operating System: %s", Platform::GetOSDisplayStr().c_str());
	LOG("        Hardware Config: %s", Platform::GetHardwareStr().c_str());
	LOG("       Binary Word Size: %s", Platform::GetWordSizeStr().c_str());
	LOG("          Process Clock: %s", spring_clock::GetName());
	LOG("     Physical CPU Cores: %d", Threading::GetPhysicalCpuCores());
	LOG("      Logical CPU Cores: %d", Threading::GetLogicalCpuCores());
	LOG("============== </User System> ==============\n");
}

void CLogOutput::LogExceptionInfo(const char* src, const char* msg)
{
	LOG_L(L_ERROR, "[%s] exception \"%s\"", src, msg);
}

