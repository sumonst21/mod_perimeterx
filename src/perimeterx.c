#include "perimeterx.h"

void set_call_reason(request_context *ctx, validation_result_t vr) {
    switch(vr) {
        case NULL_COOKIE:
            ctx->call_reason = NO_COOKIE;
            break;
        case INVALID:
            ctx->call_reason = INVALID_COOKIE;
            break;
        case EXPIRED:
            ctx->call_reason = EXPIRED_COOKIE;
            break;
    }
}

static void post_verification(request_context *ctx, px_config *conf, bool request_valid) {
    char *activity = NULL;
    char *activity_type = request_valid ? PAGE_REQUESTED_ACTIVITY_TYPE : BLOCKED_ACTIVITY_TYPE;
    activity = create_activity(activity_type, conf, ctx);
    if (activity_type == BLOCKED_ACTIVITY_TYPE || conf->send_page_activities) {
        if (send_activity(activity, conf->auth_header, ctx->r, conf->curl) != REQ_SUCCESS) {
            ERROR(ctx->r->server, "Activity: %s send failed", activity_type);
        }
    }
}

static bool px_verify_request(request_context *ctx, px_config *conf) {
    bool request_valid = true;
    risk_response *risk_response;

    if (conf->captcha_enabled && ctx->px_captcha) {
        if (verify_captcha(ctx, conf)) {
            post_verification(ctx, conf, true);
            return request_valid;
        }
    }

    validation_result_t vr = NULL_COOKIE;
    risk_cookie *c = decode_cookie(ctx->px_cookie, conf->cookie_key, ctx);
    if (c) {
        ctx->score = c->b_val;
        ctx->vid = c->vid;
        ctx->uuid = c->uuid;
        vr = validate_cookie(c, ctx, conf->cookie_key);
    }
    switch (vr) {
        case VALID:
            request_valid = ctx->score < conf->blocking_score;
            if (!request_valid) {
                ctx->block_reason = COOKIE;
            }
            break;
        case NULL_COOKIE:
        case INVALID:
        case EXPIRED:
            set_call_reason(ctx, vr);
            risk_response = risk_api_verify((const request_context*) ctx, conf);
            if (risk_response) {
                ctx->score = risk_response->score;
                request_valid = ctx->score < conf->blocking_score;
                if (!request_valid) {
                    ctx->block_reason = SERVER;
                }
            } else {
                ERROR(ctx->r->server, "Could not complete risk_api request");
            }
            break;
        default:
            ERROR(ctx->r->server, "Cookie decode failed returning valid result: %d", vr);
            break;
    }

    post_verification(ctx, conf, request_valid);
    return request_valid;
}

static bool px_should_handle_request(request_rec *r, px_config *conf) {
    static const char* file_ext_whitelist[] = { ".css", ".bmp", ".tif", ".ttf", ".docx", ".woff2", ".js", ".pict", ".tiff", ".eot", ".xlsx", ".jpg", ".csv",
        ".eps", ".woff", ".xls", ".jpeg", ".doc", ".ejs", ".otf", ".pptx", ".gif", ".pdf", ".swf", ".svg", ".ps", ".ico", ".pls", ".midi", ".svgz",
        ".class", ".png", ".ppt", ".mid", "webp", ".jar" };

    if (!conf->module_enabled) {
        return false;
    }

    const char ch = '.';
    const char *file_ending = strchr(r->uri, ch);
    if (!file_ending || strcmp(file_ending, ".html") == 0) {
        return true;
    }
    for (int i = 0; i < EXT_ARR_SIZE; i++ ) {
        if (strcmp(file_ending, file_ext_whitelist[i]) == 0) {
            return false;
        }
    }
    return true;
}

static int px_handle_requset(request_rec *r, px_config *conf) {
    bool request_valid = true;
    request_context *ctx;

    if (px_should_handle_request(r, conf)) {
        ctx = create_context(r, conf->ip_header_key);
        request_valid = px_verify_request(ctx, conf);
        apr_table_set(r->subprocess_env, "SCORE", apr_itoa(r->pool, ctx->score));
    }

    if (!request_valid) {
        char *block_page = apr_palloc(r->pool, BUF_SIZE);
        if (conf->captcha_enabled) {
            get_captcha_blocking_page(ctx, block_page);
        } else {
            get_blocking_page(ctx, block_page);
        }
        ap_rprintf(r, "%s", block_page);
        INFO(r->server, "Request Invalid");
        return DONE;
    }
    INFO(r->server, "Request Valid");
    return OK;
}


int get_blocking_page(request_context *ctx, char *buffer) {
    return sprintf(buffer, "<html lang=\"en\">\n\
               <head>\n\
                  <link type=\"text/css\" rel=\"stylesheet\" media=\"screen, print\" href=\"//fonts.googleapis.com/css?family=Open+Sans:300italic,400italic,600italic,700italic,800italic,400,300,600,700,800\">\n\
                  <meta charset=\"UTF-8\">\n\
                  <title>Access to This Page Has Been Blocked</title>\n\
                  <style> p { width: 60%; margin: 0 auto; font-size: 35px; } body { background-color: #a2a2a2; font-family: \"Open Sans\"; margin: 5%; } img { width: 180px; } a { color: #2020B1; text-decoration: blink; } a:hover { color: #2b60c6; } </style>\n\
               </head>\n\
               <body cz-shortcut-listen=\"true\">\n\
                  <div><img src=\"http://storage.googleapis.com/instapage-thumbnails/035ca0ab/e94de863/1460594818-1523851-467x110-perimeterx.png\"> </div>\n \
                  <span style=\"color: white; font-size: 34px;\">Access to This Page Has Been Blocked</span> \n\
                  <div style=\"font-size: 24px;color: #000042;\">\n\
                     <br> Access to '%s' is blocked according to the site security policy.<br> Your browsing behaviour fingerprinting made us think you may be a bot. <br> <br> This may happen as a result of the following: \n\
                     <ul>\n\
                        <li>JavaScript is disabled or not running properly.</li>\n\
                        <li>Your browsing behaviour fingerprinting are not likely to be a regular user.</li>\n\
                     </ul>\n\
                     To read more about the bot defender solution: <a href=\"https://www.perimeterx.com/bot-defender\">https://www.perimeterx.com/bot-defender</a><br> If you think the blocking was done by mistake, contact the site administrator. <br> \n\
                     <br><span style=\"font-size: 20px;\">Block Reference: <span style=\"color: #525151;\">#'%s'</span></span> \n\
                  </div>\n\
               </body>\n\
            </html>", ctx->full_url, ctx->uuid);
}

int get_captcha_blocking_page(request_context *ctx, char *buffer) {
    return sprintf(buffer, "<html lang=\"en\">\n \
            <head>\n \
                <link type=\"text/css\" rel=\"stylesheet\" media=\"screen, print\" href=\"//fonts.googleapis.com/css?family=Open+Sans:300italic,400italic,600italic,700italic,800italic,400,300,600,700,800\">\n\
                <meta charset=\"UTF-8\">\n \
                <title>Access to This Page Has Been Blocked</title>\n \
                <style> p { width: 60%; margin: 0 auto; font-size: 35px; } body { background-color: #a2a2a2; font-family: \"Open Sans\"; margin: 5%; } img { width: 180px; } a { color: #2020B1; text-decoration: blink; } a:hover { color: #2b60c6; } </style>\n \
                <script src=\"https://www.google.com/recaptcha/api.js\"></script> \
                <script> \
                    window.px_vid = '%s';\n \
                    function handleCaptcha(response) { \n \
                        var name = '_pxCaptcha';\n \
                        var expiryUtc = new Date(Date.now() + 1000 * 10).toUTCString();\n \
                        var cookieParts = [name, '=', response + ':' + window.px_vid, '; expires=', expiryUtc, '; path=/'];\n \
                        document.cookie = cookieParts.join('');\n \
                        location.reload();\n \
                    }\n \
                </script> \n \
            </head>\n \
                <body cz-shortcut-listen=\"true\">\n \
                <div><img src=\"http://storage.googleapis.com/instapage-thumbnails/035ca0ab/e94de863/1460594818-1523851-467x110-perimeterx.png\"> </div>\n \
                <span style=\"color: white; font-size: 34px;\">Access to This Page Has Been Blocked</span> \n \
                <div style=\"font-size: 24px;color: #000042;\">\n \
                <br> Access to '%s' is blocked according to the site security policy.<br> Your browsing behaviour fingerprinting made us think you may be a bot. <br> <br> This may happen as a result of the following: \n \
                <ul>\n \
                    <li>JavaScript is disabled or not running properly.</li>\n \
                    <li>Your browsing behaviour fingerprinting are not likely to be a regular user.</li>\n \
                </ul>\n \
                To read more about the bot defender solution: <a href=\"https://www.perimeterx.com/bot-defender\">https://www.perimeterx.com/bot-defender</a><br> If you think the blocking was done by mistake, contact the site administrator. <br> \n \
                <div class=\"g-recaptcha\" data-sitekey=\"6Lcj-R8TAAAAABs3FrRPuQhLMbp5QrHsHufzLf7b\" data-callback=\"handleCaptcha\" data-theme=\"dark\"></div>\n \
                <br><span style=\"font-size: 20px;\">Block Reference: <span style=\"color: #525151;\">#' %s '</span></span> \n \
                </div>\n \
            </body>\n \
            </html>", ctx->vid, ctx->full_url, ctx->uuid);
}