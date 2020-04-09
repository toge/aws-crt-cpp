#include "CanaryApp.h"
#include "CanaryUtil.h"
#include "MeasureTransferRate.h"
#include "MetricsPublisher.h"
#include "S3ObjectTransport.h"

#include <aws/crt/Api.h>
#include <aws/crt/JsonObject.h>
#include <aws/crt/Types.h>
#include <aws/crt/auth/Credentials.h>

#include <aws/common/log_channel.h>
#include <aws/common/log_formatter.h>
#include <aws/common/log_writer.h>

#ifndef WIN32
#    include <sys/resource.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

using namespace Aws::Crt;

namespace
{
    const char *MetricNamespace = "CRT-CPP-Canary-V2";
    const char *DefaultBucket = "aws-crt-canary-bucket";
} // namespace

CanaryAppOptions::CanaryAppOptions() noexcept
    : platformName(CanaryUtil::GetPlatformName()), toolName("NA"), instanceType("unknown"), region("us-west-2"),
      readFromParentPipe(-1), writeToParentPipe(-1), numUpTransfers(1), numUpConcurrentTransfers(0),
      numDownTransfers(1), numDownConcurrentTransfers(0), childProcessIndex(0), measureSinglePartTransfer(false),
      measureMultiPartTransfer(false), measureHttpTransfer(false), usingNumaControl(false), downloadOnly(false),
      sendEncrypted(false), loggingEnabled(false), rehydrateBackup(false), forkModeEnabled(false),
      isParentProcess(false), isChildProcess(false)
{
}

CanaryAppChildProcess::CanaryAppChildProcess() noexcept : pid(0), readFromChildPipe(-1), writeToChildPipe(-1) {}

CanaryAppChildProcess::CanaryAppChildProcess(pid_t inPid, int32_t inReadPipe, int32_t inWritePipe) noexcept
    : pid(inPid), readFromChildPipe(inReadPipe), writeToChildPipe(inWritePipe)
{
}

CanaryApp::CanaryApp(CanaryAppOptions &&inOptions, std::vector<CanaryAppChildProcess> &&inChildren) noexcept
    : m_options(inOptions), m_apiHandle(g_allocator),
      m_eventLoopGroup((!inOptions.isChildProcess && !inOptions.isParentProcess) ? 72 : 2, g_allocator),
      m_defaultHostResolver(m_eventLoopGroup, 60, 3600, g_allocator),
      m_bootstrap(m_eventLoopGroup, m_defaultHostResolver, g_allocator), children(inChildren)
{
#ifndef WIN32
    rlimit fdsLimit;
    getrlimit(RLIMIT_NOFILE, &fdsLimit);
    fdsLimit.rlim_cur = 8192;
    setrlimit(RLIMIT_NOFILE, &fdsLimit);
#endif

    const size_t KB_256 = 256 * 1024;
    g_aws_channel_max_fragment_size = KB_256;

    if (m_options.loggingEnabled)
    {
        m_apiHandle.InitializeLogging(LogLevel::Info, stderr);
    }

    Auth::CredentialsProviderChainDefaultConfig chainConfig;
    chainConfig.Bootstrap = &m_bootstrap;

    m_credsProvider = Auth::CredentialsProvider::CreateCredentialsProviderChainDefault(chainConfig, g_allocator);

    m_signer = MakeShared<Auth::Sigv4HttpRequestSigner>(g_allocator, g_allocator);

    Io::TlsContextOptions tlsContextOptions = Io::TlsContextOptions::InitDefaultClient(g_allocator);
    m_tlsContext = Io::TlsContext(tlsContextOptions, Io::TlsMode::CLIENT, g_allocator);

    const char *downloadBucket =
        m_options.downloadBucketName.length() > 0 ? m_options.downloadBucketName.c_str() : DefaultBucket;

    m_publisher = MakeShared<MetricsPublisher>(g_allocator, *this, MetricNamespace);
    m_uploadTransport = MakeShared<S3ObjectTransport>(g_allocator, *this, DefaultBucket);
    m_downloadTransport = MakeShared<S3ObjectTransport>(g_allocator, *this, downloadBucket);
    m_measureTransferRate = MakeShared<MeasureTransferRate>(g_allocator, *this);
}

void CanaryApp::WriteToChildProcess(uint32_t index, const char *key, const char *value)
{
#ifndef WIN32
    const CanaryAppChildProcess &child = children[index];

    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY, "Writing %s:%s to child %d through pipe %d", key, value, index, child.writeToChildPipe);

    WriteKeyValueToPipe(key, value, child.writeToChildPipe);
#else
    AWS_FATAL_ASSERT(false);
#endif
}

void CanaryApp::WriteToParentProcess(const char *key, const char *value)
{
#ifndef WIN32
    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY, "Writing %s:%s to parent through pipe %d", key, value, m_options.writeToParentPipe);

    WriteKeyValueToPipe(key, value, m_options.writeToParentPipe);
#else
    AWS_FATAL_ASSERT(false);
#endif
}

String CanaryApp::ReadFromChildProcess(uint32_t index, const char *key)
{
#ifndef WIN32
    CanaryAppChildProcess &child = children[index];

    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY,
        "Reading value of %s from child %d through pipe %d...",
        key,
        index,
        child.readFromChildPipe);

    String value = ReadValueFromPipe(key, child.readFromChildPipe, child.valuesFromChild);

    AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "Got value %s from child %d", value.c_str(), index);

    return value;
#else
    AWS_FATAL_ASSERT(false);
    return "";
#endif
}

String CanaryApp::ReadFromParentProcess(const char *key)
{
#ifndef WIN32
    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY, "Reading value of %s from parent through pipe %d...", key, m_options.readFromParentPipe);

    String value = ReadValueFromPipe(key, m_options.readFromParentPipe, valuesFromParent);

    AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "Got value %s from parent", value.c_str());

    return value;
#else
    AWS_FATAL_ASSERT(false);
    return "";
#endif
}

#ifndef WIN32

void CanaryApp::WriteKeyValueToPipe(const char *key, const char *value, uint32_t writePipe)
{
    const char nullTerm = '\0';

    write(writePipe, key, strlen(key));
    write(writePipe, &nullTerm, 1);
    write(writePipe, value, strlen(value));
    write(writePipe, &nullTerm, 1);
}

String CanaryApp::ReadValueFromPipe(const char *key, int32_t readPipe, std::map<String, String> &keyValuePairs)
{
    auto it = keyValuePairs.find(key);

    if (it != keyValuePairs.end())
    {
        return it->second;
    }

    std::pair<String, String> keyValuePair;

    do
    {
        keyValuePair = ReadNextKeyValuePairFromPipe(readPipe);
        keyValuePairs.insert(keyValuePair);

    } while (keyValuePair.first != key);

    return keyValuePair.second;
}

std::pair<String, String> CanaryApp::ReadNextKeyValuePairFromPipe(int32_t readPipe)
{
    char c;
    String currentBuffer;
    std::pair<String, String> keyValuePair;
    uint32_t index = 0;

    while (index < 2)
    {
        int32_t readResult = read(readPipe, &c, 1);

        if (readResult == -1)
        {
            AWS_LOGF_ERROR(AWS_LS_CRT_CPP_CANARY, "Read returned error %d", readResult);
            break;
        }

        if (readResult == 0)
        {
            continue;
        }

        if (c == '\0')
        {
            if (index == 0)
            {
                keyValuePair.first = std::move(currentBuffer);
            }
            else
            {
                keyValuePair.second = std::move(currentBuffer);
            }

            ++index;
        }
        else
        {
            currentBuffer += c;
        }
    }

    return keyValuePair;
}

#endif

void CanaryApp::Run()
{
    if (m_options.rehydrateBackup)
    {
        m_publisher->RehydrateBackup(m_options.rehydrateBackupObjectName.c_str());
    }

    if (m_options.measureSinglePartTransfer)
    {
        m_publisher->SetMetricTransferType(MetricTransferType::SinglePart);
        m_measureTransferRate->MeasureSinglePartObjectTransfer();
    }

    if (m_options.measureMultiPartTransfer)
    {
        m_publisher->SetMetricTransferType(MetricTransferType::MultiPart);
        m_measureTransferRate->MeasureMultiPartObjectTransfer();
    }

    if (m_options.measureHttpTransfer)
    {
        m_publisher->SetMetricTransferType(MetricTransferType::SinglePart);
        m_measureTransferRate->MeasureHttpTransfer();
    }

#ifndef WIN32
    for (CanaryAppChildProcess &childProcess : children)
    {
        if (childProcess.readFromChildPipe != -1)
        {
            close(childProcess.readFromChildPipe);
            childProcess.readFromChildPipe = -1;
        }

        if (childProcess.writeToChildPipe != -1)
        {
            close(childProcess.writeToChildPipe);
            childProcess.writeToChildPipe = -1;
        }
    }

    if (m_options.readFromParentPipe != -1)
    {
        close(m_options.readFromParentPipe);
        m_options.readFromParentPipe = -1;
    }

    if (m_options.writeToParentPipe != -1)
    {
        close(m_options.writeToParentPipe);
        m_options.writeToParentPipe = -1;
    }

    children.clear();
#endif
}