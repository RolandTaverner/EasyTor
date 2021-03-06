#pragma once

#include <string>

#include <boost/enable_shared_from_this.hpp>
#include <boost/noncopyable.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/function/function0.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/shared_lock_guard.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "pion/scheduler.hpp"

#include "Options/IConfigScheme.h"
#include "Options/IOptionsStorage.h"
#include "Process/IProcess.h"
#include "Process/ProcessConfiguration.h"

#include "Tools/Configuration/ConfigurationView.h"

namespace boost
{
namespace process
{
class child;
}
}

typedef boost::shared_ptr<boost::process::child> ChildProcessPtr;

class ProcessBase : 
    public IProcess, public ISubstitutor,
    public boost::enable_shared_from_this<ProcessBase>,
    private boost::noncopyable
{
public:
    ProcessBase(const Tools::Configuration::ConfigurationView &conf, pion::scheduler &scheduler);
    virtual ~ProcessBase();

    // IProcess
    const std::string &name() const override;

    const std::string &executable() const override;

    const boost::filesystem::path &rootPath() const override;

    const boost::filesystem::path &dataRootPath() const override;

    const ProcessConfiguration &getConfiguration() const override;

    void start(const StartProcessHandler &handler) override;

    void stop(const StopProcessHandler &handler) override;
    
    std::string cmdLineConfigName() const override;

    std::string fileConfigName() const override;
    
    bool isRunning() const override;

    bool hasConfigFile() const override;

    void getConfigurations(std::list<std::string> &configs) const override;

    void getConfigurationOptions(const std::string &configName, std::list<std::string> &options) const override;

    OptionDesc getOptionDesc(const std::string &configName, const std::string &optionName) const override;

    OptionDescValue getOptionValue(const std::string &configName, const std::string &optionName) const override;

    OptionDescValue setOptionValue(const std::string &configName,
        const std::string &optionName,
        const OptionValueContainer &optionValue) override;

    OptionDescValue removeOptionValue(const std::string &configName, const std::string &optionName) override;
    
    ProcessState getState() const override;

    ExitStatus getExitStatus() const override;

    void applyConfig(const ProcessConfiguration &presetConf) override;

    void applyUserConfig(const ProcessConfiguration &presetConf) override;

    void getLog(LogLineHandler &acc) override;

    // ISubstitutor
    bool hasSubstitute(const std::string &value) const override;
    
    std::string substituteValue(const std::string &value) const override;

    static const char *s_configFileSection;
    static const char *s_cmdLineSection;

protected:
    virtual void onProcessStart(const StartProcessHandler &handler, const std::error_code &ec);
    virtual void onProcessExit(int exitCode, const std::error_code &ec);

private:

    bool isRunningInternal() const;
    void setState(ProcessState newState);
    void applyConfigImpl(const ProcessConfiguration &presetConf, bool saveCurrentOptions);

    boost::filesystem::path createConfigFile();

    pion::scheduler &m_scheduler;
    std::string m_name;
    std::string m_executable;
    std::string m_cmdLineFixedArgs;
    boost::filesystem::path m_rootPath;
    boost::filesystem::path m_dataRootPath;
    ProcessConfiguration m_configuration;
    volatile ProcessState m_state;

    typedef boost::function0<std::string> SubsituteHandler;
    typedef std::map<std::string, SubsituteHandler> SubstituteHandlers;
    SubstituteHandlers m_substituteHandlers;

    static std::string substitutePID();
    std::string substituteRootPath() const;
    std::string substituteDataRootPath() const;
    std::string substituteConfigFilePath() const;
    
    enum class LogFilePathPart { Full, Name, Location};
    std::string substituteLogFilePath(LogFilePathPart part) const;

    boost::filesystem::path m_configFilePath;
    boost::filesystem::path m_logFilePath;
    ChildProcessPtr m_childPtr;
    StopProcessHandler m_stopHandler;

    int m_exitCode;
    std::error_code m_exitErrorCode;
    bool m_unexpectedExit;

    typedef boost::shared_mutex MutexType;
    typedef boost::shared_lock_guard<MutexType> SharedLockType;
    typedef boost::lock_guard<MutexType> UniqueLockType;

    mutable MutexType m_access;
};

typedef boost::shared_ptr<ProcessBase> ProcessBasePtr;