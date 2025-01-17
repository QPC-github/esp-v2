// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/envoy/http/service_control/client_cache.h"

#include "source/common/tracing/http_tracer_impl.h"
#include "src/api_proxy/service_control/check_response_convert_utils.h"
#include "src/api_proxy/service_control/request_builder.h"
#include "src/envoy/http/service_control/http_call.h"

namespace espv2 {
namespace envoy {
namespace http_filters {
namespace service_control {

using ::espv2::api::envoy::v11::http::service_control::FilterConfig;
using ::google::protobuf::util::OkStatus;
using ::google::protobuf::util::Status;
using ::google::protobuf::util::StatusCode;

using ::espv2::api_proxy::service_control::CheckResponseInfo;
using ::espv2::api_proxy::service_control::QuotaResponseInfo;
using ::espv2::api_proxy::service_control::ScResponseErrorType;
using ::espv2::api_proxy::service_control::api_key::ApiKeyState;
using ::google::api::servicecontrol::v1::AllocateQuotaRequest;
using ::google::api::servicecontrol::v1::AllocateQuotaResponse;
using ::google::api::servicecontrol::v1::CheckRequest;
using ::google::api::servicecontrol::v1::CheckResponse;
using ::google::api::servicecontrol::v1::ReportRequest;
using ::google::api::servicecontrol::v1::ReportResponse;

using ::google::service_control_client::CheckAggregationOptions;
using ::google::service_control_client::QuotaAggregationOptions;
using ::google::service_control_client::ReportAggregationOptions;
using ::google::service_control_client::ServiceControlClientOptions;
using ::google::service_control_client::TransportDoneFunc;

namespace {

// Default config for check aggregator
constexpr uint32_t kCheckAggregationEntries = 10000;
// We don't support quota in the check call. A check call only checks its
// api-key. It is safe to increase the check cache "flush_interval" and
// "expiration".
//
// * FlushInterval (5m): the first request hits the cache item needs
// to make a check call. But the other requests after it can continue
// to use old cached results until the check call is responded.
//
// * Expiration (1h): the cache item is purged after this.
constexpr uint32_t kCheckAggregationFlushIntervalMs = (5 * 60 * 1000);
constexpr uint32_t kCheckAggregationExpirationMs = (60 * 60 * 1000);

// Default config for quota aggregator
constexpr uint32_t kQuotaAggregationEntries = 10000;
constexpr uint32_t kQuotaAggregationFlushIntervalMs = 1000;

// Default config for report aggregator
constexpr uint32_t kReportAggregationEntries = 10000;
constexpr uint32_t kReportAggregationFlushIntervalMs = 1000;

// The default connection timeout for check requests.
constexpr uint32_t kCheckDefaultTimeoutInMs = 1000;
// The default connection timeout for allocate quota requests.
constexpr uint32_t kAllocateQuotaDefaultTimeoutInMs = 1000;
// The default connection timeout for report requests.
constexpr uint32_t kReportDefaultTimeoutInMs = 2000;

// The default number of retries for check calls.
constexpr uint32_t kCheckDefaultNumberOfRetries = 3;
// The default number of retries for allocate quota calls.
// Allocate quota has fail_open policy, retry once is enough.
constexpr uint32_t kAllocateQuotaDefaultNumberOfRetries = 1;
// The default number of retries for report calls.
constexpr uint32_t kReportDefaultNumberOfRetries = 5;

// The default value for network_fail_open flag.
constexpr bool kDefaultNetworkFailOpen = true;

// Convert http error status into the ScResponseError.
api_proxy::service_control::ScResponseError failCallStatusToScResponseError(
    const Status& status) {
  return {
      absl::StatusCodeToString(static_cast<absl::StatusCode>(status.code())),
      /*is_network_error=*/true, ScResponseErrorType::ERROR_TYPE_UNSPECIFIED};
}

// Generates CheckAggregationOptions.
CheckAggregationOptions getCheckAggregationOptions() {
  return CheckAggregationOptions(kCheckAggregationEntries,
                                 kCheckAggregationFlushIntervalMs,
                                 kCheckAggregationExpirationMs);
}

// Generates QuotaAggregationOptions.
QuotaAggregationOptions getQuotaAggregationOptions() {
  return QuotaAggregationOptions(kQuotaAggregationEntries,
                                 kQuotaAggregationFlushIntervalMs);
}

// Generates ReportAggregationOptions.
ReportAggregationOptions getReportAggregationOptions() {
  return ReportAggregationOptions(kReportAggregationEntries,
                                  kReportAggregationFlushIntervalMs);
}

// A timer object to wrap PeriodicTimer
class EnvoyPeriodicTimer
    : public ::google::service_control_client::PeriodicTimer {
 public:
  EnvoyPeriodicTimer(Envoy::Event::Dispatcher& dispatcher, int interval_ms,
                     std::function<void()> callback)
      : interval_ms_(interval_ms),
        callback_(callback),
        timer_(dispatcher.createTimer([this]() { call(); })) {
    timer_->enableTimer(std::chrono::milliseconds(interval_ms_));
  }

  void call() {
    callback_();
    timer_->enableTimer(std::chrono::milliseconds(interval_ms_));
  }

  // Cancels the timer.
  virtual void Stop() override { timer_.reset(); }

 private:
  int interval_ms_;
  std::function<void()> callback_;
  Envoy::Event::TimerPtr timer_;
};

}  // namespace

template <class Response>
Status ClientCache::processScCallTransportStatus(const Status& status,
                                                 Response* resp,
                                                 const std::string& body) {
  std::string callName;
  if (std::is_same<Response, CheckResponse>::value) {
    callName = "check";
  } else if (std::is_same<Response, AllocateQuotaResponse>::value) {
    callName = "allocateQuota";
  } else if (std::is_same<Response, ReportResponse>::value) {
    callName = "report";
  }

  if (!status.ok()) {
    ENVOY_LOG(error, "Failed to call {}, error: {}, str body: {}", callName,
              status.ToString(), body);
  } else {
    if (!resp->ParseFromString(body)) {
      ENVOY_LOG(error, "Failed to call {}, error: {}, str body: {}", callName,
                "invalid response", body);
      return Status(StatusCode::kInvalidArgument,
                    std::string("Invalid response"));
    }
  }

  return status;
}

void ClientCache::initHttpRequestSetting(const FilterConfig& filter_config) {
  if (!filter_config.has_sc_calling_config()) {
    network_fail_open_ = kDefaultNetworkFailOpen;
    check_timeout_ms_ = kCheckDefaultTimeoutInMs;
    quota_timeout_ms_ = kAllocateQuotaDefaultTimeoutInMs;
    report_timeout_ms_ = kReportDefaultTimeoutInMs;
    check_retries_ = kCheckDefaultNumberOfRetries;
    quota_retries_ = kAllocateQuotaDefaultNumberOfRetries;
    report_retries_ = kReportDefaultNumberOfRetries;
    return;
  }
  const auto& sc_calling_config = filter_config.sc_calling_config();
  network_fail_open_ = sc_calling_config.has_network_fail_open()
                           ? sc_calling_config.network_fail_open().value()
                           : true;
  check_timeout_ms_ = sc_calling_config.has_check_timeout_ms()
                          ? sc_calling_config.check_timeout_ms().value()
                          : kCheckDefaultTimeoutInMs;
  quota_timeout_ms_ = sc_calling_config.has_quota_timeout_ms()
                          ? sc_calling_config.quota_timeout_ms().value()
                          : kAllocateQuotaDefaultTimeoutInMs;
  report_timeout_ms_ = sc_calling_config.has_report_timeout_ms()
                           ? sc_calling_config.report_timeout_ms().value()
                           : kReportDefaultTimeoutInMs;

  check_retries_ = sc_calling_config.has_check_retries()
                       ? sc_calling_config.check_retries().value()
                       : kCheckDefaultNumberOfRetries;
  quota_retries_ = sc_calling_config.has_quota_retries()
                       ? sc_calling_config.quota_retries().value()
                       : kAllocateQuotaDefaultNumberOfRetries;
  report_retries_ = sc_calling_config.has_report_retries()
                        ? sc_calling_config.report_retries().value()
                        : kReportDefaultNumberOfRetries;
}

void ClientCache::collectCallStatus(CallStatusStats& call_stats,
                                    const StatusCode& code) {
  ServiceControlFilterStats::collectCallStatus(call_stats, code);
}

ClientCache::ClientCache(
    const ::espv2::api::envoy::v11::http::service_control::Service& config,
    const FilterConfig& filter_config, const std::string& stats_prefix,
    Envoy::Stats::Scope& scope, Envoy::Upstream::ClusterManager& cm,
    Envoy::TimeSource& time_source, Envoy::Event::Dispatcher& dispatcher,
    std::function<const std::string&()> sc_token_fn,
    std::function<const std::string&()> quota_token_fn)
    : config_(config),
      filter_stats_(ServiceControlFilterStats::create(stats_prefix, scope)),
      time_source_(time_source) {
  ServiceControlClientOptions options(getCheckAggregationOptions(),
                                      getQuotaAggregationOptions(),
                                      getReportAggregationOptions());

  initHttpRequestSetting(filter_config);
  check_call_factory_ = std::make_unique<HttpCallFactoryImpl>(
      cm, dispatcher, filter_config.service_control_uri(),
      absl::StrCat("/", config_.service_name(), ":check"), sc_token_fn,
      check_timeout_ms_, check_retries_, time_source,
      "Service Control remote call: Check");
  quota_call_factory_ = std::make_unique<HttpCallFactoryImpl>(
      cm, dispatcher, filter_config.service_control_uri(),
      absl::StrCat("/", config_.service_name(), ":allocateQuota"),
      quota_token_fn, quota_timeout_ms_, quota_retries_, time_source,
      "Service Control remote call: Allocate Quota");
  report_call_factory_ = std::make_unique<HttpCallFactoryImpl>(
      cm, dispatcher, filter_config.service_control_uri(),
      absl::StrCat("/", config_.service_name(), ":report"), sc_token_fn,
      report_timeout_ms_, report_retries_, time_source,
      "Service Control remote call: Report");

  // Note: Check transport is also defined per request.
  // But this must be defined, it will be called on each flush of the cache
  // entry. This occurs on periodic timer and cache destruction.
  options.check_transport = [this](const CheckRequest& request,
                                   CheckResponse* response,
                                   TransportDoneFunc on_done) {
    // Don't support tracing on this transport
    auto& null_span = Envoy::Tracing::NullSpan::instance();
    auto* call = check_call_factory_->createHttpCall(
        request, null_span,
        [this, response, on_done](const Status& status,
                                  const std::string& body) {
          Status final_status = processScCallTransportStatus<CheckResponse>(
              status, response, body);
          collectCallStatus(filter_stats_.check_, final_status.code());
          on_done(final_status);
        });
    call->call();
  };

  options.quota_transport = [this](const AllocateQuotaRequest& request,
                                   AllocateQuotaResponse* response,
                                   TransportDoneFunc on_done) {
    // Don't support tracing on this transport
    auto& null_span = Envoy::Tracing::NullSpan::instance();
    auto* call = quota_call_factory_->createHttpCall(
        request, null_span,
        [this, response, on_done](const Status& status,
                                  const std::string& body) {
          Status final_status =
              processScCallTransportStatus<AllocateQuotaResponse>(
                  status, response, body);
          collectCallStatus(filter_stats_.allocate_quota_, final_status.code());
          on_done(final_status);
        });
    call->call();
  };

  options.report_transport = [this](const ReportRequest& request,
                                    ReportResponse* response,
                                    TransportDoneFunc on_done) {
    // Don't support tracing on this transport
    auto& null_span = Envoy::Tracing::NullSpan::instance();
    auto* call = report_call_factory_->createHttpCall(
        request, null_span,
        [this, response, on_done](const Status& status,
                                  const std::string& body) {
          Status final_status = processScCallTransportStatus<ReportResponse>(
              status, response, body);
          collectCallStatus(filter_stats_.report_, final_status.code());

          on_done(final_status);
        });
    call->call();
  };

  options.periodic_timer = [&dispatcher](int interval_ms,
                                         std::function<void()> callback)
      -> std::unique_ptr<::google::service_control_client::PeriodicTimer> {
    return std::unique_ptr<::google::service_control_client::PeriodicTimer>(
        new EnvoyPeriodicTimer(dispatcher, interval_ms, callback));
  };

  client_ = ::google::service_control_client::CreateServiceControlClient(
      config_.service_name(), config_.service_config_id(), options);
}

void ClientCache::collectScResponseErrorStats(ScResponseErrorType error_type) {
  switch (error_type) {
    case ScResponseErrorType::CONSUMER_BLOCKED:
      filter_stats_.filter_.denied_consumer_blocked_.inc();
      break;
    case ScResponseErrorType::CONSUMER_ERROR:
    case ScResponseErrorType::SERVICE_NOT_ACTIVATED:
    case ScResponseErrorType::API_KEY_INVALID:
      filter_stats_.filter_.denied_consumer_error_.inc();
      break;
    case ScResponseErrorType::CONSUMER_QUOTA:
      filter_stats_.filter_.denied_consumer_quota_.inc();
    default:
      break;
  }
}

CancelFunc ClientCache::callCheck(const CheckRequest& request,
                                  Envoy::Tracing::Span& parent_span,
                                  CheckDoneFunc on_done) {
  CancelFunc cancel_fn;
  auto check_transport = [this, &parent_span, &cancel_fn](
                             const CheckRequest& request,
                             CheckResponse* response,
                             TransportDoneFunc on_done) {
    auto* call = check_call_factory_->createHttpCall(
        request, parent_span,
        [this, response, on_done](const Status& status,
                                  const std::string& body) {
          Status final_status = processScCallTransportStatus<CheckResponse>(
              status, response, body);
          collectCallStatus(filter_stats_.check_, final_status.code());
          on_done(final_status);
        });
    call->call();
    cancel_fn = [call]() { call->cancel(); };
  };

  parent_span.log(time_source_.systemTime(),
                  "Service Control cache query: Check");

  auto* response = new CheckResponse;
  client_->Check(
      request, response,
      [this, response, on_done](const Status& http_status) {
        handleCheckResponse(http_status, response, on_done);
      },
      check_transport);
  return cancel_fn;
}

void ClientCache::handleCheckResponse(const Status& http_status,
                                      CheckResponse* response,
                                      CheckDoneFunc on_done) {
  CheckResponseInfo response_info;
  Status final_status;

  if (http_status.ok()) {
    // If the http call succeeded, then use the CheckResponseInfo
    // to retrieve the final status.
    final_status = api_proxy::service_control::ConvertCheckResponse(
        *response, config_.service_name(), &response_info);
    collectScResponseErrorStats(response_info.error.type);

  } else {
    // Otherwise, http call failed. Use that status to respond.
    final_status = http_status;
  }

  if (final_status.ok()) {
    // Everything succeeded, API Key is trusted.
    response_info.api_key_state = ApiKeyState::VERIFIED;
    on_done(final_status, response_info);
  } else if (final_status.code() == StatusCode::kUnavailable) {
    // All 5xx errors are already translated to Unavailable.
    // API Key cannot be trusted due to a network error.
    response_info.api_key_state = ApiKeyState::NOT_CHECKED;

    if (network_fail_open_) {
      filter_stats_.filter_.allowed_control_plane_fault_.inc();
      ENVOY_LOG(warn,
                "Google Service Control Check is unavailable, but the "
                "request is allowed due to network fail open. Original "
                "error: {}",
                final_status.message());
      on_done(OkStatus(), response_info);
    } else {
      // Preserve the original 5xx error code in the response back.
      filter_stats_.filter_.denied_control_plane_fault_.inc();
      ENVOY_LOG(warn,
                "Google Service Control Check is unavailable, and the "
                "request is denied due to network fail closed, with error: {}",
                final_status.message());

      // If http_status is not ok, the StatusCode::kUnavailable is from
      // http_status.
      if (!http_status.ok()) {
        response_info.error = failCallStatusToScResponseError(http_status);
      }
      on_done(final_status, response_info);
    }
  } else {
    if (!http_status.ok()) {
      // Most likely an auth error in ESPv2 or API producer deployment.
      filter_stats_.filter_.denied_producer_error_.inc();

      // API Key cannot be trusted due to network error with Service Control.
      response_info.api_key_state = ApiKeyState::NOT_CHECKED;

      // This is not caused by a client request error, so translate
      // non-5xx error codes to 500 Internal Server Error. Error message
      // contains details on the original error (including the original
      // HTTP status code).
      Status scrubbed_status(StatusCode::kInternal, final_status.message());

      response_info.error = failCallStatusToScResponseError(http_status);
      on_done(scrubbed_status, response_info);
    } else {
      // HTTP succeeded, but SC Check returned 4xx.
      // Stats already incremented for this case.

      // Handle API Key validity.
      if (response_info.error.type == ScResponseErrorType::API_KEY_INVALID) {
        response_info.api_key_state = ApiKeyState::INVALID;
      } else if (response_info.error.type ==
                 ScResponseErrorType::SERVICE_NOT_ACTIVATED) {
        response_info.api_key_state = ApiKeyState::NOT_ENABLED;
      } else {
        // All other SC Check errors assume consumer was identified.
        response_info.api_key_state = ApiKeyState::VERIFIED;
      }

      on_done(final_status, response_info);
    }
  }
  delete response;
}

void ClientCache::callQuota(const AllocateQuotaRequest& request,
                            QuotaDoneFunc on_done) {
  auto* response = new AllocateQuotaResponse;
  client_->Quota(request, response,
                 [this, response, on_done](const Status& status) {
                   // Configured to always use the quota cache, so the status
                   // will always be OK. Response message is from the cache. If
                   // a cache miss occurs or the quota server is unavailable
                   // during cache refresh, the status will still be OK and the
                   // response message will be empty. This is also treated as a
                   // success.
                   handleQuotaOnDone(status, response, on_done);
                 });
}

void ClientCache::handleQuotaOnDone(const Status& http_status,
                                    AllocateQuotaResponse* response,
                                    QuotaDoneFunc on_done) {
  QuotaResponseInfo response_info;
  if (http_status.ok()) {
    Status quota_status =
        ::espv2::api_proxy::service_control::ConvertAllocateQuotaResponse(
            *response, config_.service_name(), &response_info);

    collectScResponseErrorStats(response_info.error.type);
    on_done(quota_status, response_info);
  } else {
    // Most likely an auth error in ESPv2 or API producer deployment.
    filter_stats_.filter_.denied_producer_error_.inc();

    response_info.error = failCallStatusToScResponseError(http_status);
    on_done(http_status, response_info);
  }

  delete response;
}

void ClientCache::callReport(const ReportRequest& request) {
  auto* response = new ReportResponse;
  client_->Report(request, response,
                  [response](const Status&) { delete response; });
}

}  // namespace service_control
}  // namespace http_filters
}  // namespace envoy
}  // namespace espv2
