#include <gtest/gtest.h>

#include "zhttp/mid/auth_middleware.h"
#include "zhttp/router.h"
#include "zhttp/session.h"

using namespace zhttp;
using namespace zhttp::mid;

namespace {

class AuthenticationMiddlewareTest : public ::testing::Test {
  protected:
    HttpRequest::ptr make_get_request(const std::string &path) {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::GET);
        req->set_path(path);
        return req;
    }

    std::shared_ptr<Session> make_session_with(const std::string &key,
                                               const std::string &value) {
        auto session = std::make_shared<Session>("sid-test", false);
        session->set(key, value);
        session->mark_persisted();
        return session;
    }
};

class RoleAuthorizationMiddlewareTest : public ::testing::Test {
  protected:
    HttpRequest::ptr make_get_request(const std::string &path) {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::GET);
        req->set_path(path);
        return req;
    }

    std::shared_ptr<Session> make_session_with(const std::string &key,
                                               const std::string &value) {
        auto session = std::make_shared<Session>("sid-test", false);
        session->set(key, value);
        session->mark_persisted();
        return session;
    }
};

} // namespace

TEST_F(AuthenticationMiddlewareTest, RejectWhenNoCredential) {
    Router router;
    AuthenticationMiddleware::Options options;
    options.use_session = true;
    options.session_auth_key = "user_id";
    options.use_bearer_token = false;

    router.use(std::make_shared<AuthenticationMiddleware>(options));

    bool handler_called = false;
    router.get("/secure", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/secure");
    HttpResponse resp;
    router.route(req, resp);

    EXPECT_FALSE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    EXPECT_EQ(resp.body_content(), "Unauthorized");
}

TEST_F(AuthenticationMiddlewareTest, AllowWhenSessionCredentialExists) {
    Router router;
    AuthenticationMiddleware::Options options;
    options.use_session = true;
    options.session_auth_key = "user_id";
    options.use_bearer_token = false;

    router.use(std::make_shared<AuthenticationMiddleware>(options));

    bool handler_called = false;
    router.get("/secure", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/secure");
    req->set_session(make_session_with("user_id", "1001"));

    HttpResponse resp;
    router.route(req, resp);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
    EXPECT_EQ(resp.body_content(), "ok");
}

TEST_F(AuthenticationMiddlewareTest, AllowWhenBearerTokenValid) {
    Router router;
    AuthenticationMiddleware::Options options;
    options.use_session = false;
    options.use_bearer_token = true;
    options.token_validator = [](const std::string &token,
                                 const HttpRequest::ptr &) {
        return token == "valid-token";
    };

    router.use(std::make_shared<AuthenticationMiddleware>(options));

    bool handler_called = false;
    router.get("/secure", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/secure");
    req->set_header("Authorization", "Bearer valid-token");

    HttpResponse resp;
    router.route(req, resp);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
}

TEST_F(AuthenticationMiddlewareTest, HandlesBearerEdgeCasesAndCustomUnauthorized) {
    AuthenticationMiddleware::Options options;
    options.use_session = true;
    options.session_auth_key = "user_id";
    options.use_bearer_token = true;
    options.token_validator = [](const std::string &token,
                                 const HttpRequest::ptr &) {
        return token == "valid-token";
    };
    options.unauthorized_handler = [](HttpResponse &resp) {
        resp.status(HttpStatus::UNAUTHORIZED).text("custom-unauthorized");
    };

    AuthenticationMiddleware middleware(options);

    {
        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Bad");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
        EXPECT_EQ(resp.body_content(), "custom-unauthorized");
    }

    {
        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Token valid-token");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    }

    {
        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Bearer    ");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    }

    {
        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Bearer   valid-token   ");
        HttpResponse resp;
        EXPECT_TRUE(middleware.before(req, resp));
    }

    {
        auto req = make_get_request("/secure");
        req->set_session(make_session_with("user_id", ""));
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    }
}

TEST_F(AuthenticationMiddlewareTest,
       BearerAuthFailsWhenValidatorMissingOrFeatureDisabled) {
    {
        AuthenticationMiddleware::Options options;
        options.use_session = false;
        options.use_bearer_token = true;
        options.token_validator = nullptr;
        AuthenticationMiddleware middleware(options);

        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Bearer token");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    }

    {
        AuthenticationMiddleware::Options options;
        options.use_session = false;
        options.use_bearer_token = false;
        options.token_validator = [](const std::string &, const HttpRequest::ptr &) {
            return true;
        };
        AuthenticationMiddleware middleware(options);

        auto req = make_get_request("/secure");
        req->set_header("Authorization", "Bearer token");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::UNAUTHORIZED);
    }
}

TEST_F(RoleAuthorizationMiddlewareTest, AllowWhenAnyRoleMatches) {
    Router router;
    router.use(std::make_shared<RoleAuthorizationMiddleware>(
        std::vector<std::string>{"admin", "ops"}));

    bool handler_called = false;
    router.get("/admin", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/admin");
    req->set_session(make_session_with("roles", "viewer, admin"));

    HttpResponse resp;
    router.route(req, resp);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
}

TEST_F(RoleAuthorizationMiddlewareTest, RejectWhenRequireAllButMissingRole) {
    Router router;
    RoleAuthorizationMiddleware::Options options;
    options.require_all = true;

    router.use(std::make_shared<RoleAuthorizationMiddleware>(
        std::vector<std::string>{"admin", "ops"}, options));

    bool handler_called = false;
    router.get("/ops", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/ops");
    req->set_session(make_session_with("roles", "admin"));

    HttpResponse resp;
    router.route(req, resp);

    EXPECT_FALSE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
    EXPECT_EQ(resp.body_content(), "Forbidden");
}

TEST_F(RoleAuthorizationMiddlewareTest, CaseInsensitiveMatchByDefault) {
    Router router;
    router.use(std::make_shared<RoleAuthorizationMiddleware>(
        std::vector<std::string>{"ADMIN"}));

    bool handler_called = false;
    router.get("/panel", [&](const HttpRequest::ptr &, HttpResponse &resp) {
        handler_called = true;
        resp.text("ok");
    });

    auto req = make_get_request("/panel");
    req->set_session(make_session_with("roles", "admin"));

    HttpResponse resp;
    router.route(req, resp);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
}

TEST_F(RoleAuthorizationMiddlewareTest, EmptyRequiredRolesAlwaysAllow) {
    RoleAuthorizationMiddleware middleware(std::vector<std::string>{});
    auto req = make_get_request("/open");
    HttpResponse resp;
    EXPECT_TRUE(middleware.before(req, resp));
}

TEST_F(RoleAuthorizationMiddlewareTest,
       UsesRoleResolverAndNormalizesRolesBeforeAuthorization) {
    RoleAuthorizationMiddleware::Options options;
    options.role_resolver = [](const HttpRequest::ptr &) {
        return std::vector<std::string>{"  ADMIN  ", " ", "viewer"};
    };
    RoleAuthorizationMiddleware middleware({"admin"}, options);

    auto req = make_get_request("/panel");
    HttpResponse resp;
    EXPECT_TRUE(middleware.before(req, resp));
}

TEST_F(RoleAuthorizationMiddlewareTest,
       CaseSensitiveModeAndCustomForbiddenHandlerAreApplied) {
    RoleAuthorizationMiddleware::Options options;
    options.case_sensitive = true;
    options.forbidden_handler = [](HttpResponse &resp) {
        resp.status(HttpStatus::FORBIDDEN).text("custom-forbidden");
    };
    RoleAuthorizationMiddleware middleware({"Admin"}, options);

    {
        auto req = make_get_request("/admin");
        req->set_session(make_session_with("roles", "admin"));
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
        EXPECT_EQ(resp.body_content(), "custom-forbidden");
    }

    {
        auto req = make_get_request("/admin");
        req->set_session(make_session_with("roles", "Admin"));
        HttpResponse resp;
        EXPECT_TRUE(middleware.before(req, resp));
    }
}

TEST_F(RoleAuthorizationMiddlewareTest,
       RequireAllModePassesOnlyWhenAllRolesPresent) {
    RoleAuthorizationMiddleware::Options options;
    options.require_all = true;
    RoleAuthorizationMiddleware middleware({"admin", "ops"}, options);

    {
        auto req = make_get_request("/ops");
        req->set_session(make_session_with("roles", "admin, ops"));
        HttpResponse resp;
        EXPECT_TRUE(middleware.before(req, resp));
    }

    {
        auto req = make_get_request("/ops");
        req->set_session(make_session_with("roles", ""));
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
