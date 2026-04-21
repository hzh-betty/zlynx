#include <gtest/gtest.h>
#include <thread>

#include "zhttp/mid/session_middleware.h"
#include "zhttp/router.h"
#include "zhttp/session.h"

using namespace zhttp;
using namespace zhttp::mid;

static std::string extract_sid(const std::string &set_cookie_value) {
    // ZHTTPSESSID=<id>; ...
    auto eq = set_cookie_value.find('=');
    if (eq == std::string::npos) {
        return "";
    }
    auto semi = set_cookie_value.find(';', eq + 1);
    if (semi == std::string::npos) {
        return set_cookie_value.substr(eq + 1);
    }
    return set_cookie_value.substr(eq + 1, semi - (eq + 1));
}

class SessionTest : public ::testing::Test {
  protected:
    SessionManager::ptr mgr_ = std::make_shared<SessionManager>(
        SessionManager::Options{std::chrono::seconds(60), 1});

    SessionMiddleware::Options base_opt() {
        SessionMiddleware::Options opt;
        opt.cookie_name = "ZHTTPSESSID";
        opt.create_if_missing = true;
        return opt;
    }

    HttpRequest::ptr make_get(const std::string &path,
                              const std::string &cookie = "") {
        auto r = std::make_shared<HttpRequest>();
        r->set_method(HttpMethod::GET);
        r->set_path(path);
        if (!cookie.empty()) {
            r->set_header("Cookie", cookie);
        }
        return r;
    }
};

TEST_F(SessionTest, CookieRoundTrip) {
    Router router;
    router.use(std::make_shared<SessionMiddleware>(mgr_, base_opt()));

    router.get("/s", [](const HttpRequest::ptr &req, HttpResponse &resp) {
        ASSERT_NE(req->session(), nullptr);
        req->session()->set("k", "v");
        resp.text("ok");
    });

    HttpResponse resp1;
    router.route(make_get("/s"), resp1);

    ASSERT_FALSE(resp1.set_cookies().empty());
    std::string sid = extract_sid(resp1.set_cookies().front());
    ASSERT_FALSE(sid.empty());

    router.get("/s2", [](const HttpRequest::ptr &req, HttpResponse &resp) {
        ASSERT_NE(req->session(), nullptr);
        EXPECT_EQ(req->session()->get("k"), "v");
        resp.text("ok2");
    });

    HttpResponse resp2;
    router.route(make_get("/s2", "ZHTTPSESSID=" + sid), resp2);
    EXPECT_TRUE(resp2.set_cookies().empty());
}

TEST_F(SessionTest, CreateIfMissingFalse_DoesNotCreateSession) {
    SessionMiddleware::Options opt = base_opt();
    opt.create_if_missing = false;

    Router router;
    router.use(std::make_shared<SessionMiddleware>(mgr_, opt));

    router.get("/s", [](const HttpRequest::ptr &req, HttpResponse &resp) {
        EXPECT_EQ(req->session(), nullptr);
        resp.text("ok");
    });

    HttpResponse resp;
    router.route(make_get("/s"), resp);
    EXPECT_TRUE(resp.set_cookies().empty());
}

TEST_F(SessionTest, ExistingSessionSettingSameValue_DoesNotTriggerSetCookie) {
    Router router;
    router.use(std::make_shared<SessionMiddleware>(mgr_, base_opt()));

    router.get("/init", [](const HttpRequest::ptr &req, HttpResponse &resp) {
        ASSERT_NE(req->session(), nullptr);
        req->session()->set("k", "v");
        resp.text("init");
    });

    HttpResponse init_resp;
    router.route(make_get("/init"), init_resp);
    ASSERT_FALSE(init_resp.set_cookies().empty());
    std::string sid = extract_sid(init_resp.set_cookies().front());
    ASSERT_FALSE(sid.empty());

    router.get("/noop", [](const HttpRequest::ptr &req, HttpResponse &resp) {
        ASSERT_NE(req->session(), nullptr);
        EXPECT_EQ(req->session()->get("k"), "v");
        req->session()->set("k", "v");
        resp.text("noop");
    });

    HttpResponse noop_resp;
    router.route(make_get("/noop", "ZHTTPSESSID=" + sid), noop_resp);
    EXPECT_TRUE(noop_resp.set_cookies().empty());
}

TEST(SessionCoreTest, SessionMutationFlagsHandleNoopAndEraseClearPaths) {
    Session session("sid", true);
    EXPECT_EQ(session.get("missing", "default"), "default");
    EXPECT_TRUE(session.is_new());
    EXPECT_FALSE(session.modified());

    session.set("k", "v");
    EXPECT_TRUE(session.modified());
    session.mark_persisted();
    EXPECT_FALSE(session.is_new());
    EXPECT_FALSE(session.modified());

    session.set("k", "v");
    EXPECT_FALSE(session.modified());

    session.erase("missing");
    EXPECT_FALSE(session.modified());

    session.erase("k");
    EXPECT_TRUE(session.modified());
    session.mark_persisted();
    EXPECT_FALSE(session.modified());

    session.clear();
    EXPECT_FALSE(session.modified());

    session.set("a", "1");
    session.mark_persisted();
    session.clear();
    EXPECT_TRUE(session.modified());
}

TEST(SessionManagerCoreTest, HandlesLoadCreateSaveDestroyAndExpiration) {
    SessionManager manager(SessionManager::Options{std::chrono::seconds(1), 1});

    EXPECT_EQ(manager.load(""), nullptr);
    EXPECT_EQ(manager.load("missing"), nullptr);

    auto session = manager.create();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->id().size(), 32u);

    session->set("k", "v");
    manager.save(*session);

    auto loaded = manager.load(session->id());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->get("k"), "v");

    manager.destroy(session->id());
    EXPECT_EQ(manager.load(session->id()), nullptr);

    auto expiring = manager.create();
    ASSERT_NE(expiring, nullptr);
    expiring->set("x", "y");
    manager.save(*expiring);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_EQ(manager.load(expiring->id()), nullptr);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
