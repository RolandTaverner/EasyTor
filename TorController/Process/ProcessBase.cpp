#include <windows.h>

#include <fstream>

#include <boost/assert.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/function/function0.hpp>
#include <boost/function/function1.hpp>
#include <boost/function/function2.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/process.hpp>
#include <boost/process/extend.hpp>

#include "Tools/Logger/Logger.h"

#include "Options/ConfigScheme.h"
#include "Options/OptionsStorage.h"
#include "Process/ProcessBase.h"
#include "Process/ProcessErrors.h"

namespace bp = boost::process;
namespace ex = bp::extend;

const char *ProcessBase::s_configFileSection = "config";
const char *ProcessBase::s_cmdLineSection = "cmdline";

//-------------------------------------------------------------------------------------------------
ProcessBase::ProcessBase(const Tools::Configuration::ConfigurationView &conf, pion::scheduler &scheduler) :
    m_scheduler(scheduler),
    m_state(ProcessState::Stopped),
    m_exitCode(0),
    m_unexpectedExit(false)
{
    m_name = conf.getAttr("", "name");
    m_executable = conf.get("executable");
    m_rootPath = conf.get("root");
    m_cmdLineFixedArgs = conf.get("args", std::string());

    BOOST_FOREACH(const Tools::Configuration::ConfigurationView &schemeConf, conf.getRangeOf("options.scheme"))
    {
        const std::string name = schemeConf.getAttr("", "name");
        IConfigSchemePtr scheme = ConfigScheme::createFromConfig(schemeConf);
        IOptionsStoragePtr storage(new OptionsStorage(scheme));
        m_configuration.addStorage(name, storage);
    }

    m_substituteHandlers["PID"] = &ProcessBase::substitutePID;
    m_substituteHandlers["ROOTPATH"] = boost::bind(&ProcessBase::substituteRootPath, this);
    m_substituteHandlers["CONFIGFILE"] = boost::bind(&ProcessBase::substituteConfigFilePath, this);

    m_scheduler.add_active_user();
}

//-------------------------------------------------------------------------------------------------
ProcessBase::~ProcessBase()
{
    m_scheduler.remove_active_user();
}

//-------------------------------------------------------------------------------------------------
const std::string &ProcessBase::name() const
{
    return m_name;
}

//-------------------------------------------------------------------------------------------------
const std::string &ProcessBase::executable() const
{
    return m_executable;
}

//-------------------------------------------------------------------------------------------------
const boost::filesystem::path &ProcessBase::rootPath() const
{
    return m_rootPath;
}

//-------------------------------------------------------------------------------------------------
const ProcessConfiguration &ProcessBase::getConfiguration() const
{
    return m_configuration;
}

//-------------------------------------------------------------------------------------------------
class AsyncProcessHandler : public ex::async_handler
{
public:
    typedef boost::function2<void, int, const std::error_code &> OnExitHandler;
    typedef boost::function0<void> OnSuccessHandler;
    typedef boost::function1<void, const std::error_code &> OnErrorHandler;

    AsyncProcessHandler(const OnExitHandler &onExit,
        const OnSuccessHandler &onSuccess,
        const OnErrorHandler &onError):
        m_onExit(onExit), m_onSuccess(onSuccess), m_onError(onError) {}

    virtual ~AsyncProcessHandler() {}

    template<typename Executor>
    std::function<void(int, const std::error_code&)> on_exit_handler(Executor &exec)
    {
        auto handler = this->m_onExit;
        return [handler](int exitCode, const std::error_code &ec)
        {
            handler(exitCode, ec);
        };
    }

    template <class Executor>
    void on_setup(Executor &exec) const 
    {
        exec.creation_flags |= CREATE_NO_WINDOW;
    }

    template <class Executor>
    void on_error(Executor &exec, const std::error_code &ec) const
    {
        boost::asio::io_service &ios = ex::get_io_service(exec.seq);
        ios.post(boost::bind<>(m_onError, ec));
    }

    template <class Executor>
    void on_success(Executor &exec) const 
    {
        boost::asio::io_service &ios = ex::get_io_service(exec.seq);
        ios.post(m_onSuccess);
    }

private:
    OnExitHandler m_onExit;
    OnSuccessHandler m_onSuccess;
    OnErrorHandler m_onError;
};

//-------------------------------------------------------------------------------------------------
void ProcessBase::start(const StartProcessHandler &handler)
{
	UniqueLockType lock(m_access);

	if (isRunningInternal()) {
		throw ProcessError(makeErrorCode(ProcessErrors::alreadyRunning));
	}

	if (hasConfigFile())
	{
        createConfigFile();
	}

    const std::string exePath = (rootPath() / executable()).string();

    std::vector<std::string> args;

    if (!m_cmdLineFixedArgs.empty())
    {
        args.push_back(m_cmdLineFixedArgs);
    }
	
    if (m_configuration.hasStorage(s_cmdLineSection))
    {
        IOptionsStoragePtr configPtr = m_configuration.getStorage(s_cmdLineSection);
        IConfigSchemePtr schemePtr = configPtr->getScheme();

        for (const OptionDesc &od : schemePtr->getFilteredRange<>(OptionIsRequiredPred()))
        {
            const std::string &name = od.get<0>();
            if (!configPtr->hasValue(name))
            {
                throw ProcessError(makeErrorCode(ProcessErrors::missingRequiredOption), "Required option " + name + " has no value.");
            }
        }

        BOOST_FOREACH(const Option &o, configPtr->getRange())
        {const std::string formattedString = configPtr->formatOption(o.name(), shared_from_this());
            args.push_back(formattedString);
        }

    }
    std::string commandArgs = exePath;
    for (const std::string &i : args)
    {
        commandArgs += " " + i;
    }

    Tools::Logger::Logger::getInstance().info("Starting process " + commandArgs);
    
    setState(ProcessState::Starting);
    m_exitCode = 0;
    m_exitErrorCode = std::error_code();
    m_unexpectedExit = false;

    m_childPtr.reset(new bp::child(bp::exe = exePath,
        bp::args = args, m_scheduler.get_io_service(),
        AsyncProcessHandler(boost::bind<>(&ProcessBase::onProcessExit, shared_from_this(), _1, _2),
            boost::bind<>(&ProcessBase::onProcessStart, shared_from_this(), handler, std::error_code()),
            boost::bind<>(&ProcessBase::onProcessStart, shared_from_this(), handler, _1))
        ));
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::onProcessStart(const StartProcessHandler &handler, const std::error_code &ec)
{
    UniqueLockType lock(m_access);

    if (ec)
    {
        setState(ProcessState::Stopped);
        m_childPtr.reset();
    }
    else
    {
        setState(ProcessState::Running);
    }
    handler(ec);
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::onProcessExit(int exitCode, const std::error_code &ec)
{
    UniqueLockType lock(m_access);
    if (m_state != ProcessState::Stopping)
    {
        m_unexpectedExit = true;
    }
    m_exitCode = exitCode;
    m_exitErrorCode = ec;
    setState(ProcessState::Stopped);
    m_childPtr.reset();
    if (!m_stopHandler.empty())
    {
        m_stopHandler(ec, ExitStatus(m_exitCode, ec, m_unexpectedExit));
        m_stopHandler.clear();
    }
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::cmdLineConfigName() const
{
    return s_cmdLineSection;
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::fileConfigName() const
{
    return s_configFileSection;
}

//-------------------------------------------------------------------------------------------------
bool ProcessBase::isRunning() const
{
	SharedLockType lock(m_access);

	return isRunningInternal();
}

//-------------------------------------------------------------------------------------------------
bool ProcessBase::isRunningInternal() const
{
	return m_childPtr && m_childPtr->running();
}

//-------------------------------------------------------------------------------------------------
bool ProcessBase::hasConfigFile() const
{
	return m_configuration.hasStorage(s_configFileSection);
}

//-------------------------------------------------------------------------------------------------
boost::filesystem::path ProcessBase::createConfigFile()
{
    BOOST_ASSERT(hasConfigFile());
    
    IOptionsStoragePtr configPtr = m_configuration.getStorage(s_configFileSection);
    IConfigSchemePtr schemePtr = configPtr->getScheme();
    
    for (const OptionDesc &od : schemePtr->getFilteredRange<>(OptionIsRequiredPred()))
    {
        const std::string &name = od.get<0>();
        if (!configPtr->hasValue(name))
        {
            throw ProcessError(makeErrorCode(ProcessErrors::missingRequiredOption), "Required option " + name + " has no value.");
        }
    }

    const boost::filesystem::path outFilePath = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();

    std::ofstream outFile(outFilePath.c_str(), std::ios_base::out | std::ios_base::trunc);

    if (!outFile)
    {
        throw ProcessError(makeErrorCode(ProcessErrors::configFileWriteError), "Can't create file " + outFilePath.string());
    }

    BOOST_FOREACH(const Option &o, configPtr->getRange())
    {
        const std::string formattedString = configPtr->formatOption(o.name(), shared_from_this());
        outFile << formattedString << std::endl;
    }
    outFile.close();

    m_configFilePath = outFilePath;

    return m_configFilePath;
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::getConfigurations(std::list<std::string> &configs) const
{
    SharedLockType lock(m_access);
    m_configuration.getStorages(configs);
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::getConfigurationOptions(const std::string &configName, std::list<std::string> &options) const
{
    SharedLockType lock(m_access);
    if (!m_configuration.hasStorage(configName))
    {
        throw ProcessError(makeErrorCode(ProcessErrors::noSuchStorage),
                           "Cant't find storage " + configName + " in process " + name());
    }

    IOptionsStorageConstPtr storagePtr = m_configuration.getStorage(configName);
    for (const OptionDesc &o : storagePtr->getScheme()->getRange())
    {
        options.push_back(o.get<0>());
    }
}

//-------------------------------------------------------------------------------------------------
OptionDesc ProcessBase::getOptionDesc(const std::string &configName, const std::string &optionName) const
{
    SharedLockType lock(m_access);
    if (!m_configuration.hasStorage(configName))
    {
        throw ProcessError(makeErrorCode(ProcessErrors::noSuchStorage),
                           "Cant't find storage " + configName + " in process " + name());
    }

    IOptionsStorageConstPtr storagePtr = m_configuration.getStorage(configName);

    if (!storagePtr->getScheme()->hasOption(optionName))
    {
        throw ProcessError(makeErrorCode(ProcessErrors::noSuchOption),
                           "Cant't find option " + optionName + " in storage " + configName + " in process " + name());
    }

    return storagePtr->getScheme()->getOptionDesc(optionName);
}

//-------------------------------------------------------------------------------------------------
OptionDescValue ProcessBase::getOptionValue(const std::string &configName, const std::string &optionName) const
{
    SharedLockType lock(m_access);
    if (!m_configuration.hasStorage(configName))
    {
        throw ProcessError(makeErrorCode(ProcessErrors::noSuchStorage),
            "Cant't find storage " + configName + " in process " + name());
    }

    IOptionsStorageConstPtr storagePtr = m_configuration.getStorage(configName);
    IConfigSchemePtr schemePtr = storagePtr->getScheme();

    if (!schemePtr->hasOption(optionName))
    {
        throw ProcessError(makeErrorCode(ProcessErrors::noSuchOption),
            "Cant't find option " + optionName + " in storage " + configName + " in process " + name());
    }

    OptionDescValue result;
    const OptionDesc desc = storagePtr->getScheme()->getOptionDesc(optionName);
    const OptionValueType value = storagePtr->hasValue(optionName) ? storagePtr->getValue(optionName) :
        (schemePtr->hasDefaultValue(optionName) ? schemePtr->getDefaultValue(optionName) : OptionValueType());
    const std::string presentation = storagePtr->hasValue(optionName) ? storagePtr->formatOption(optionName, shared_from_this()) : "";
    return OptionDescValue(desc, value, presentation);
}

//-------------------------------------------------------------------------------------------------
ProcessState ProcessBase::getState() const
{
    return m_state;
}

//-------------------------------------------------------------------------------------------------
bool ProcessBase::hasSubstitute(const std::string &value) const
{
    return m_substituteHandlers.find(value) != m_substituteHandlers.end();
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::substituteValue(const std::string &value) const
{
    SubstituteHandlers::const_iterator it = m_substituteHandlers.find(value);
    if (it == m_substituteHandlers.end())
    {
        throw ProcessError(makeErrorCode(ProcessErrors::substitutionNotFound), "Process " + name() + " has no substitution for " + value);
    }
    return (it->second)();
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::substitutePID()
{
    return boost::lexical_cast<std::string>(GetCurrentProcessId());
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::substituteRootPath() const
{
    return rootPath().string();
}

//-------------------------------------------------------------------------------------------------
std::string ProcessBase::substituteConfigFilePath() const
{
    return m_configFilePath.string();
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::setState(ProcessState newState)
{
    m_state = newState;
}

//-------------------------------------------------------------------------------------------------
ExitStatus ProcessBase::getExitStatus() const
{
    SharedLockType lock(m_access);
    return ExitStatus(m_exitCode, m_exitErrorCode, m_unexpectedExit);
}

//-------------------------------------------------------------------------------------------------
void ProcessBase::stop(const StopProcessHandler &handler)
{
    UniqueLockType lock(m_access);

    if (!isRunningInternal() || getState() != ProcessState::Running)
    {
        throw ProcessError(makeErrorCode(ProcessErrors::processNotRunning));
    }

    BOOST_ASSERT(m_stopHandler.empty());

    m_stopHandler = handler;
    setState(ProcessState::Stopping);
    std::error_code ec;
    m_childPtr->terminate(ec);
}