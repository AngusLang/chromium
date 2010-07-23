// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy/proxy_service.h"

#include <algorithm>

#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/histogram.h"
#include "base/message_loop.h"
#include "base/string_util.h"
#include "googleurl/src/gurl.h"
#include "net/base/forwarding_net_log.h"
#include "net/base/net_log.h"
#include "net/base/net_errors.h"
#include "net/base/net_util.h"
#include "net/proxy/init_proxy_resolver.h"
#include "net/proxy/multi_threaded_proxy_resolver.h"
#include "net/proxy/proxy_config_service_fixed.h"
#include "net/proxy/proxy_script_fetcher.h"
#if defined(OS_WIN)
#include "net/proxy/proxy_config_service_win.h"
#include "net/proxy/proxy_resolver_winhttp.h"
#elif defined(OS_MACOSX)
#include "net/proxy/proxy_config_service_mac.h"
#include "net/proxy/proxy_resolver_mac.h"
#elif defined(OS_LINUX)
#include "net/proxy/proxy_config_service_linux.h"
#endif
#include "net/proxy/proxy_resolver.h"
#include "net/proxy/proxy_resolver_js_bindings.h"
#include "net/proxy/proxy_resolver_v8.h"
#include "net/proxy/sync_host_resolver_bridge.h"
#include "net/url_request/url_request_context.h"

using base::TimeDelta;
using base::TimeTicks;

namespace net {

static const size_t kMaxNumNetLogEntries = 100;
static const size_t kDefaultNumPacThreads = 4;

// Config getter that always returns direct settings.
class ProxyConfigServiceDirect : public ProxyConfigService {
 public:
  // ProxyConfigService implementation:
  virtual void AddObserver(Observer* observer) {}
  virtual void RemoveObserver(Observer* observer) {}
  virtual bool GetLatestProxyConfig(ProxyConfig* config) {
    *config = ProxyConfig::CreateDirect();
    return true;
  }
};

// Proxy resolver that fails every time.
class ProxyResolverNull : public ProxyResolver {
 public:
  ProxyResolverNull() : ProxyResolver(false /*expects_pac_bytes*/) {}

  // ProxyResolver implementation:
  virtual int GetProxyForURL(const GURL& url,
                             ProxyInfo* results,
                             CompletionCallback* callback,
                             RequestHandle* request,
                             const BoundNetLog& net_log) {
    return ERR_NOT_IMPLEMENTED;
  }

  virtual void CancelRequest(RequestHandle request) {
    NOTREACHED();
  }

  virtual int SetPacScript(
      const scoped_refptr<ProxyResolverScriptData>& /*script_data*/,
      CompletionCallback* /*callback*/) {
    return ERR_NOT_IMPLEMENTED;
  }
};

// This factory creates V8ProxyResolvers with appropriate javascript bindings.
class ProxyResolverFactoryForV8 : public ProxyResolverFactory {
 public:
  // |async_host_resolver|, |io_loop| and |net_log| must remain
  // valid for the duration of our lifetime.
  // Both |async_host_resolver| and |net_log| will only be operated on
  // |io_loop|.
  ProxyResolverFactoryForV8(HostResolver* async_host_resolver,
                            MessageLoop* io_loop,
                            NetLog* net_log)
      : ProxyResolverFactory(true /*expects_pac_bytes*/),
        async_host_resolver_(async_host_resolver),
        io_loop_(io_loop),
        forwarding_net_log_(
            net_log ? new ForwardingNetLog(net_log, io_loop) : NULL) {
  }

  virtual ProxyResolver* CreateProxyResolver() {
    // Create a synchronous host resolver wrapper that operates
    // |async_host_resolver_| on |io_loop_|.
    SyncHostResolverBridge* sync_host_resolver =
        new SyncHostResolverBridge(async_host_resolver_, io_loop_);

    ProxyResolverJSBindings* js_bindings =
        ProxyResolverJSBindings::CreateDefault(sync_host_resolver,
                                               forwarding_net_log_.get());

    // ProxyResolverV8 takes ownership of |js_bindings|.
    return new ProxyResolverV8(js_bindings);
  }

 private:
  scoped_refptr<HostResolver> async_host_resolver_;
  MessageLoop* io_loop_;

  // Thread-safe wrapper around a non-threadsafe NetLog implementation. This
  // enables the proxy resolver to emit log messages from the PAC thread.
  scoped_ptr<ForwardingNetLog> forwarding_net_log_;
};

// Creates ProxyResolvers using a non-V8 implementation.
class ProxyResolverFactoryForNonV8 : public ProxyResolverFactory {
 public:
  ProxyResolverFactoryForNonV8()
      : ProxyResolverFactory(false /*expects_pac_bytes*/) {}

  virtual ProxyResolver* CreateProxyResolver() {
#if defined(OS_WIN)
    return new ProxyResolverWinHttp();
#elif defined(OS_MACOSX)
    return new ProxyResolverMac();
#else
    LOG(WARNING) << "PAC support disabled because there is no fallback "
                    "non-V8 implementation";
    return new ProxyResolverNull();
#endif
  }
};

// ProxyService::PacRequest ---------------------------------------------------

class ProxyService::PacRequest
    : public base::RefCounted<ProxyService::PacRequest> {
 public:
  PacRequest(ProxyService* service,
             const GURL& url,
             ProxyInfo* results,
             CompletionCallback* user_callback,
             const BoundNetLog& net_log)
      : service_(service),
        user_callback_(user_callback),
        ALLOW_THIS_IN_INITIALIZER_LIST(io_callback_(
            this, &PacRequest::QueryComplete)),
        results_(results),
        url_(url),
        resolve_job_(NULL),
        config_id_(ProxyConfig::INVALID_ID),
        net_log_(net_log) {
    DCHECK(user_callback);
  }

  // Starts the resolve proxy request.
  int Start() {
    DCHECK(!was_cancelled());
    DCHECK(!is_started());

    DCHECK(service_->config_.is_valid());

    config_id_ = service_->config_.id();

    return resolver()->GetProxyForURL(
        url_, results_, &io_callback_, &resolve_job_, net_log_);
  }

  bool is_started() const {
    // Note that !! casts to bool. (VS gives a warning otherwise).
    return !!resolve_job_;
  }

  void StartAndCompleteCheckingForSynchronous() {
    int rv = service_->TryToCompleteSynchronously(url_, results_);
    if (rv == ERR_IO_PENDING)
      rv = Start();
    if (rv != ERR_IO_PENDING)
      QueryComplete(rv);
  }

  void CancelResolveJob() {
    DCHECK(is_started());
    // The request may already be running in the resolver.
    resolver()->CancelRequest(resolve_job_);
    resolve_job_ = NULL;
    DCHECK(!is_started());
  }

  void Cancel() {
    net_log_.AddEvent(NetLog::TYPE_CANCELLED, NULL);

    if (is_started())
      CancelResolveJob();

    // Mark as cancelled, to prevent accessing this again later.
    service_ = NULL;
    user_callback_ = NULL;
    results_ = NULL;

    net_log_.EndEvent(NetLog::TYPE_PROXY_SERVICE, NULL);
  }

  // Returns true if Cancel() has been called.
  bool was_cancelled() const { return user_callback_ == NULL; }

  // Helper to call after ProxyResolver completion (both synchronous and
  // asynchronous). Fixes up the result that is to be returned to user.
  int QueryDidComplete(int result_code) {
    DCHECK(!was_cancelled());

    // Make a note in the results which configuration was in use at the
    // time of the resolve.
    results_->config_id_ = config_id_;

    // Reset the state associated with in-progress-resolve.
    resolve_job_ = NULL;
    config_id_ = ProxyConfig::INVALID_ID;

    return service_->DidFinishResolvingProxy(results_, result_code, net_log_);
  }

  BoundNetLog* net_log() { return &net_log_; }

 private:
  friend class base::RefCounted<ProxyService::PacRequest>;

  ~PacRequest() {}

  // Callback for when the ProxyResolver request has completed.
  void QueryComplete(int result_code) {
    result_code = QueryDidComplete(result_code);

    // Remove this completed PacRequest from the service's pending list.
    /// (which will probably cause deletion of |this|).
    CompletionCallback* callback = user_callback_;
    service_->RemovePendingRequest(this);

    callback->Run(result_code);
  }

  ProxyResolver* resolver() const { return service_->resolver_.get(); }

  // Note that we don't hold a reference to the ProxyService. Outstanding
  // requests are cancelled during ~ProxyService, so this is guaranteed
  // to be valid throughout our lifetime.
  ProxyService* service_;
  CompletionCallback* user_callback_;
  CompletionCallbackImpl<PacRequest> io_callback_;
  ProxyInfo* results_;
  GURL url_;
  ProxyResolver::RequestHandle resolve_job_;
  ProxyConfig::ID config_id_;  // The config id when the resolve was started.
  BoundNetLog net_log_;
};

// ProxyService ---------------------------------------------------------------

ProxyService::ProxyService(ProxyConfigService* config_service,
                           ProxyResolver* resolver,
                           NetLog* net_log)
    : resolver_(resolver),
      next_config_id_(1),
      should_use_proxy_resolver_(false),
      ALLOW_THIS_IN_INITIALIZER_LIST(init_proxy_resolver_callback_(
          this, &ProxyService::OnInitProxyResolverComplete)),
      current_state_(STATE_NONE) ,
      net_log_(net_log) {
  NetworkChangeNotifier::AddObserver(this);
  ResetConfigService(config_service);
}

// static
ProxyService* ProxyService::Create(
    ProxyConfigService* proxy_config_service,
    bool use_v8_resolver,
    size_t num_pac_threads,
    URLRequestContext* url_request_context,
    NetLog* net_log,
    MessageLoop* io_loop) {
  if (num_pac_threads == 0)
    num_pac_threads = kDefaultNumPacThreads;

  ProxyResolverFactory* sync_resolver_factory;
  if (use_v8_resolver) {
    sync_resolver_factory =
        new ProxyResolverFactoryForV8(
            url_request_context->host_resolver(),
            io_loop,
            net_log);
  } else {
    sync_resolver_factory = new ProxyResolverFactoryForNonV8();
  }

  ProxyResolver* proxy_resolver =
      new MultiThreadedProxyResolver(sync_resolver_factory, num_pac_threads);

  ProxyService* proxy_service =
      new ProxyService(proxy_config_service, proxy_resolver, net_log);

  if (proxy_resolver->expects_pac_bytes()) {
    // Configure PAC script downloads to be issued using |url_request_context|.
    DCHECK(url_request_context);
    proxy_service->SetProxyScriptFetcher(
        ProxyScriptFetcher::Create(url_request_context));
  }

  return proxy_service;
}

// static
ProxyService* ProxyService::CreateFixed(const ProxyConfig& pc) {
  // TODO(eroman): This isn't quite right, won't work if |pc| specifies
  //               a PAC script.
  return Create(new ProxyConfigServiceFixed(pc), false, 0, NULL, NULL, NULL);
}

// static
ProxyService* ProxyService::CreateNull() {
  // Use direct connections.
  return new ProxyService(new ProxyConfigServiceDirect, new ProxyResolverNull,
                          NULL);
}

int ProxyService::ResolveProxy(const GURL& raw_url,
                               ProxyInfo* result,
                               CompletionCallback* callback,
                               PacRequest** pac_request,
                               const BoundNetLog& net_log) {
  DCHECK(callback);

  net_log.BeginEvent(NetLog::TYPE_PROXY_SERVICE, NULL);

  config_service_->OnLazyPoll();
  if (current_state_ == STATE_NONE)
    ApplyProxyConfigIfAvailable();

  // Strip away any reference fragments and the username/password, as they
  // are not relevant to proxy resolution.
  GURL url = SimplifyUrlForRequest(raw_url);

  // Check if the request can be completed right away. (This is the case when
  // using a direct connection for example).
  int rv = TryToCompleteSynchronously(url, result);
  if (rv != ERR_IO_PENDING)
    return DidFinishResolvingProxy(result, rv, net_log);

  scoped_refptr<PacRequest> req =
      new PacRequest(this, url, result, callback, net_log);

  if (current_state_ == STATE_READY) {
    // Start the resolve request.
    rv = req->Start();
    if (rv != ERR_IO_PENDING)
      return req->QueryDidComplete(rv);
  } else {
    req->net_log()->BeginEvent(NetLog::TYPE_PROXY_SERVICE_WAITING_FOR_INIT_PAC,
                               NULL);
  }

  DCHECK_EQ(ERR_IO_PENDING, rv);
  DCHECK(!ContainsPendingRequest(req));
  pending_requests_.push_back(req);

  // Completion will be notifed through |callback|, unless the caller cancels
  // the request using |pac_request|.
  if (pac_request)
    *pac_request = req.get();
  return rv;  // ERR_IO_PENDING
}

int ProxyService::TryToCompleteSynchronously(const GURL& url,
                                             ProxyInfo* result) {
  DCHECK_NE(STATE_NONE, current_state_);

  if (current_state_ != STATE_READY)
    return ERR_IO_PENDING;  // Still initializing.

  if (should_use_proxy_resolver_)
    return ERR_IO_PENDING;  // Must submit the request to the proxy resolver.

  // Use the manual proxy settings.
  DCHECK(config_.id() != ProxyConfig::INVALID_ID);
  config_.proxy_rules().Apply(url, result);
  result->config_id_ = config_.id();
  return OK;
}

ProxyService::~ProxyService() {
  NetworkChangeNotifier::RemoveObserver(this);
  config_service_->RemoveObserver(this);

  // Cancel any inprogress requests.
  for (PendingRequests::iterator it = pending_requests_.begin();
       it != pending_requests_.end();
       ++it) {
    (*it)->Cancel();
  }
}

void ProxyService::SuspendAllPendingRequests() {
  for (PendingRequests::iterator it = pending_requests_.begin();
       it != pending_requests_.end();
       ++it) {
    PacRequest* req = it->get();
    if (req->is_started()) {
      req->CancelResolveJob();

      req->net_log()->BeginEvent(
          NetLog::TYPE_PROXY_SERVICE_WAITING_FOR_INIT_PAC, NULL);
    }
  }
}

void ProxyService::SetReady() {
  DCHECK(!init_proxy_resolver_.get());
  current_state_ = STATE_READY;

  // Make a copy in case |this| is deleted during the synchronous completion
  // of one of the requests. If |this| is deleted then all of the PacRequest
  // instances will be Cancel()-ed.
  PendingRequests pending_copy = pending_requests_;

  for (PendingRequests::iterator it = pending_copy.begin();
       it != pending_copy.end();
       ++it) {
    PacRequest* req = it->get();
    if (!req->is_started() && !req->was_cancelled()) {
      req->net_log()->EndEvent(NetLog::TYPE_PROXY_SERVICE_WAITING_FOR_INIT_PAC,
                               NULL);

      // Note that we re-check for synchronous completion, in case we are
      // no longer using a ProxyResolver (can happen if we fell-back to manual).
      req->StartAndCompleteCheckingForSynchronous();
    }
  }
}

void ProxyService::ApplyProxyConfigIfAvailable() {
  DCHECK_EQ(STATE_NONE, current_state_);

  current_state_ = STATE_WAITING_FOR_PROXY_CONFIG;

  config_service_->OnLazyPoll();

  // Retrieve the current proxy configuration from the ProxyConfigService.
  // If a configuration is not available yet, we will get called back later
  // by our ProxyConfigService::Observer once it changes.
  ProxyConfig config;
  bool has_config = config_service_->GetLatestProxyConfig(&config);
  if (has_config)
    OnProxyConfigChanged(config);
}

void ProxyService::OnInitProxyResolverComplete(int result) {
  DCHECK_EQ(STATE_WAITING_FOR_INIT_PROXY_RESOLVER, current_state_);
  DCHECK(init_proxy_resolver_.get());
  DCHECK(config_.MayRequirePACResolver());
  DCHECK(!should_use_proxy_resolver_);
  init_proxy_resolver_.reset();

  should_use_proxy_resolver_ = result == OK;

  if (result != OK) {
    LOG(INFO) << "Failed configuring with PAC script, falling-back to manual "
                 "proxy servers.";
  }

  // Resume any requests which we had to defer until the PAC script was
  // downloaded.
  SetReady();
}

int ProxyService::ReconsiderProxyAfterError(const GURL& url,
                                            ProxyInfo* result,
                                            CompletionCallback* callback,
                                            PacRequest** pac_request,
                                            const BoundNetLog& net_log) {
  // Check to see if we have a new config since ResolveProxy was called.  We
  // want to re-run ResolveProxy in two cases: 1) we have a new config, or 2) a
  // direct connection failed and we never tried the current config.

  bool re_resolve = result->config_id_ != config_.id();

  if (re_resolve) {
    // If we have a new config or the config was never tried, we delete the
    // list of bad proxies and we try again.
    proxy_retry_info_.clear();
    return ResolveProxy(url, result, callback, pac_request, net_log);
  }

  // We don't have new proxy settings to try, try to fallback to the next proxy
  // in the list.
  bool did_fallback = result->Fallback(&proxy_retry_info_);

  // Return synchronous failure if there is nothing left to fall-back to.
  // TODO(eroman): This is a yucky API, clean it up.
  return did_fallback ? OK : ERR_FAILED;
}

void ProxyService::CancelPacRequest(PacRequest* req) {
  DCHECK(req);
  req->Cancel();
  RemovePendingRequest(req);
}

bool ProxyService::ContainsPendingRequest(PacRequest* req) {
  PendingRequests::iterator it = std::find(
      pending_requests_.begin(), pending_requests_.end(), req);
  return pending_requests_.end() != it;
}

void ProxyService::RemovePendingRequest(PacRequest* req) {
  DCHECK(ContainsPendingRequest(req));
  PendingRequests::iterator it = std::find(
      pending_requests_.begin(), pending_requests_.end(), req);
  pending_requests_.erase(it);
}

int ProxyService::DidFinishResolvingProxy(ProxyInfo* result,
                                          int result_code,
                                          const BoundNetLog& net_log) {
  // Log the result of the proxy resolution.
  if (result_code == OK) {
    // When full logging is enabled, dump the proxy list.
    if (net_log.HasListener()) {
      net_log.AddEvent(
          NetLog::TYPE_PROXY_SERVICE_RESOLVED_PROXY_LIST,
          new NetLogStringParameter("pac_string", result->ToPacString()));
    }
    result->DeprioritizeBadProxies(proxy_retry_info_);
  } else {
    net_log.AddEvent(
        NetLog::TYPE_PROXY_SERVICE_RESOLVED_PROXY_LIST,
        new NetLogIntegerParameter("net_error", result_code));

    // Fall-back to direct when the proxy resolver fails. This corresponds
    // with a javascript runtime error in the PAC script.
    //
    // This implicit fall-back to direct matches Firefox 3.5 and
    // Internet Explorer 8. For more information, see:
    //
    // http://www.chromium.org/developers/design-documents/proxy-settings-fallback
    result->UseDirect();
    result_code = OK;
  }

  net_log.EndEvent(NetLog::TYPE_PROXY_SERVICE, NULL);
  return result_code;
}

void ProxyService::SetProxyScriptFetcher(
    ProxyScriptFetcher* proxy_script_fetcher) {
  State previous_state = ResetProxyConfig();
  proxy_script_fetcher_.reset(proxy_script_fetcher);
  if (previous_state != STATE_NONE)
    ApplyProxyConfigIfAvailable();
}

ProxyScriptFetcher* ProxyService::GetProxyScriptFetcher() const {
  return proxy_script_fetcher_.get();
}

ProxyService::State ProxyService::ResetProxyConfig() {
  State previous_state = current_state_;

  proxy_retry_info_.clear();
  init_proxy_resolver_.reset();
  should_use_proxy_resolver_ = false;
  SuspendAllPendingRequests();
  config_ = ProxyConfig();
  current_state_ = STATE_NONE;

  return previous_state;
}

void ProxyService::ResetConfigService(
    ProxyConfigService* new_proxy_config_service) {
  State previous_state = ResetProxyConfig();

  // Release the old configuration service.
  if (config_service_.get())
    config_service_->RemoveObserver(this);

  // Set the new configuration service.
  config_service_.reset(new_proxy_config_service);
  config_service_->AddObserver(this);

  if (previous_state != STATE_NONE)
    ApplyProxyConfigIfAvailable();
}

void ProxyService::PurgeMemory() {
  if (resolver_.get())
    resolver_->PurgeMemory();
}

void ProxyService::ForceReloadProxyConfig() {
  ResetProxyConfig();
  ApplyProxyConfigIfAvailable();
}

// static
ProxyConfigService* ProxyService::CreateSystemProxyConfigService(
    MessageLoop* io_loop, MessageLoop* file_loop) {
#if defined(OS_WIN)
  return new ProxyConfigServiceWin();
#elif defined(OS_MACOSX)
  return new ProxyConfigServiceMac();
#elif defined(OS_LINUX)
  ProxyConfigServiceLinux* linux_config_service
      = new ProxyConfigServiceLinux();

  // Assume we got called from the UI loop, which runs the default
  // glib main loop, so the current thread is where we should be
  // running gconf calls from.
  MessageLoop* glib_default_loop = MessageLoopForUI::current();

  // The file loop should be a MessageLoopForIO on Linux.
  DCHECK_EQ(MessageLoop::TYPE_IO, file_loop->type());

  // Synchronously fetch the current proxy config (since we are
  // running on glib_default_loop). Additionally register for
  // notifications (delivered in either |glib_default_loop| or
  // |file_loop|) to keep us updated when the proxy config changes.
  linux_config_service->SetupAndFetchInitialConfig(glib_default_loop, io_loop,
      static_cast<MessageLoopForIO*>(file_loop));

  return linux_config_service;
#else
  LOG(WARNING) << "Failed to choose a system proxy settings fetcher "
                  "for this platform.";
  return new ProxyConfigServiceNull();
#endif
}

void ProxyService::OnProxyConfigChanged(const ProxyConfig& config) {
  ResetProxyConfig();

  // Increment the ID to reflect that the config has changed.
  config_ = config;
  config_.set_id(next_config_id_++);

  if (!config_.MayRequirePACResolver()) {
    SetReady();
    return;
  }

  // Start downloading + testing the PAC scripts for this new configuration.
  current_state_ = STATE_WAITING_FOR_INIT_PROXY_RESOLVER;

  init_proxy_resolver_.reset(
      new InitProxyResolver(resolver_.get(), proxy_script_fetcher_.get(),
                            net_log_));

  int rv = init_proxy_resolver_->Init(
      config_, &init_proxy_resolver_callback_);

  if (rv != ERR_IO_PENDING)
    OnInitProxyResolverComplete(rv);
}

void ProxyService::OnIPAddressChanged() {
  State previous_state = ResetProxyConfig();
  if (previous_state != STATE_NONE)
    ApplyProxyConfigIfAvailable();
}

SyncProxyServiceHelper::SyncProxyServiceHelper(MessageLoop* io_message_loop,
                                               ProxyService* proxy_service)
    : io_message_loop_(io_message_loop),
      proxy_service_(proxy_service),
      event_(false, false),
      ALLOW_THIS_IN_INITIALIZER_LIST(callback_(
          this, &SyncProxyServiceHelper::OnCompletion)) {
  DCHECK(io_message_loop_ != MessageLoop::current());
}

int SyncProxyServiceHelper::ResolveProxy(const GURL& url,
                                         ProxyInfo* proxy_info,
                                         const BoundNetLog& net_log) {
  DCHECK(io_message_loop_ != MessageLoop::current());

  io_message_loop_->PostTask(FROM_HERE, NewRunnableMethod(
      this, &SyncProxyServiceHelper::StartAsyncResolve, url, net_log));

  event_.Wait();

  if (result_ == net::OK) {
    *proxy_info = proxy_info_;
  }
  return result_;
}

int SyncProxyServiceHelper::ReconsiderProxyAfterError(
    const GURL& url, ProxyInfo* proxy_info, const BoundNetLog& net_log) {
  DCHECK(io_message_loop_ != MessageLoop::current());

  io_message_loop_->PostTask(FROM_HERE, NewRunnableMethod(
      this, &SyncProxyServiceHelper::StartAsyncReconsider, url, net_log));

  event_.Wait();

  if (result_ == net::OK) {
    *proxy_info = proxy_info_;
  }
  return result_;
}

void SyncProxyServiceHelper::StartAsyncResolve(const GURL& url,
                                               const BoundNetLog& net_log) {
  result_ = proxy_service_->ResolveProxy(
      url, &proxy_info_, &callback_, NULL, net_log);
  if (result_ != net::ERR_IO_PENDING) {
    OnCompletion(result_);
  }
}

void SyncProxyServiceHelper::StartAsyncReconsider(const GURL& url,
                                                  const BoundNetLog& net_log) {
  result_ = proxy_service_->ReconsiderProxyAfterError(
      url, &proxy_info_, &callback_, NULL, net_log);
  if (result_ != net::ERR_IO_PENDING) {
    OnCompletion(result_);
  }
}

void SyncProxyServiceHelper::OnCompletion(int rv) {
  result_ = rv;
  event_.Signal();
}

}  // namespace net
