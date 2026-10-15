// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
// Local replacement responses follow Brave adblock_stub_response.cc (MPL-2.0)
// semantics. This implementation preserves request origin/response tainting.
#include "chrome/common/ohos/adblock/decision_throttle.h"
#include <algorithm>
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/data_url.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/content_security_policy/content_security_policy.h"
#include "services/network/public/cpp/parsed_headers.h"
#include "services/network/public/cpp/single_request_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/parsed_headers.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
namespace chrome::ohos::adblock {
namespace {
constexpr size_t kMaxReplacement = 1024 * 1024;
void Serve(std::string mime,
           std::string charset,
           std::string body,
           const network::ResourceRequest& request,
           mojo::PendingReceiver<network::mojom::URLLoader> loader,
           mojo::PendingRemote<network::mojom::URLLoaderClient> pending) {
  mojo::Remote<network::mojom::URLLoaderClient> client(std::move(pending));
  const bool same_origin =
      request.request_initiator &&
      request.request_initiator->IsSameOriginWith(request.url);
  if ((!same_origin &&
       request.mode == network::mojom::RequestMode::kSameOrigin) ||
      (request.method != "GET" && request.method != "HEAD")) {
    client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
    return;
  }
  auto head = network::mojom::URLResponseHead::New();
  head->mime_type = mime;
  head->charset = charset;
  head->content_length = body.size();
  if (request.method == "HEAD")
    body.clear();
  const std::string origin = request.request_initiator
                                 ? request.request_initiator->Serialize()
                                 : "null";
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(
          "HTTP/1.1 200 OK\r\nContent-Type: " + mime +
          (charset.empty() ? "" : "; charset=" + charset) +
          "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: " +
          origin +
          "\r\nAccess-Control-Allow-Credentials: true\r\n"
          "Cross-Origin-Resource-Policy: cross-origin\r\n"
          "X-Content-Type-Options: nosniff\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n"));
  head->response_type = same_origin ? network::mojom::FetchResponseType::kBasic
                        : request.mode == network::mojom::RequestMode::kNoCors
                            ? network::mojom::FetchResponseType::kOpaque
                            : network::mojom::FetchResponseType::kCors;
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  MojoCreateDataPipeOptions options{
      sizeof(options), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1,
      static_cast<uint32_t>(std::max<size_t>(1, body.size()))};
  if (mojo::CreateDataPipe(&options, producer, consumer) != MOJO_RESULT_OK ||
      (!body.empty() &&
       producer->WriteAllData(base::as_byte_span(body)) != MOJO_RESULT_OK)) {
    client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
    return;
  }
  producer.reset();
  client->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  status.decoded_body_length = body.size();
  status.encoded_body_length = body.size();
  client->OnComplete(status);
}
}  // namespace
void DecisionThrottle::Capture(const network::ResourceRequest& request) {
  request_ = request;
}
void DecisionThrottle::Redirect(const net::RedirectInfo& redirect) {
  request_.UpdateOnRedirect(redirect);
}
void DecisionThrottle::Resolve(mojom::DecisionPtr decision) {
  csp_ = decision ? decision->csp : "";
  if (decision && decision->blocked) {
    std::string mime, charset, body;
    // Document substitution could grant a local script the blocked site's
    // origin. Only inert/subresource replacement is supported.
    if (request_.destination != network::mojom::RequestDestination::kDocument &&
        request_.destination != network::mojom::RequestDestination::kIframe &&
        request_.destination != network::mojom::RequestDestination::kFrame &&
        !decision->redirect.empty() &&
        decision->redirect.size() <= 2 * kMaxReplacement &&
        net::DataURL::Parse(GURL(decision->redirect), &mime, &charset, &body) &&
        body.size() <= kMaxReplacement) {
      auto factory =
          base::MakeRefCounted<network::SingleRequestURLLoaderFactory>(
              base::BindOnce(&Serve, std::move(mime), std::move(charset),
                             std::move(body)));
      if (delegate_->OverrideDeferredRequest(GURL(), std::move(factory))) {
        delegate_->Resume();
        return;
      }
    }
    delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT);
    return;
  }
  if (decision && !decision->rewritten_url.is_empty()) {
    if (++rewrites_ > 8 ||
        !delegate_->OverrideDeferredRequest(decision->rewritten_url, nullptr)) {
      delegate_->CancelWithError(net::ERR_TOO_MANY_REDIRECTS);
      return;
    }
  }
  delegate_->Resume();
}
void DecisionThrottle::WillProcessResponse(
    const GURL& url,
    network::mojom::URLResponseHead* head,
    bool*) {
  if (csp_.empty() || csp_.size() > 65536 ||
      csp_.find_first_of("\r\n") != std::string::npos || !head->headers)
    return;
  // Append an independent enforcing policy; do not overwrite site CSP or
  // report-only policies. Chromium consumes both raw and parsed headers.
  if (!head->parsed_headers)
    head->parsed_headers =
        network::PopulateParsedHeaders(head->headers.get(), url);
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      head->headers->raw_headers());
  head->headers->AddHeader("Content-Security-Policy", csp_);
  if (!head->parsed_headers)
    head->parsed_headers = network::mojom::ParsedHeaders::New();
  auto policies = network::ParseContentSecurityPolicies(
      csp_, network::mojom::ContentSecurityPolicyType::kEnforce,
      network::mojom::ContentSecurityPolicySource::kHTTP, url);
  for (auto& policy : policies)
    head->parsed_headers->content_security_policy.push_back(std::move(policy));
}
}  // namespace chrome::ohos::adblock
