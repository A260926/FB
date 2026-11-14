import net from "net";
import type { OptionsInit } from "got";
import { HttpProxyAgent, HttpsProxyAgent } from "hpagent";
import http2Wrapper from "http2-wrapper";
import { cleanObject, getType, isValidUrl } from "./lib/utils.js";
import { RequestConfig, RequestOptions, CrawlerOptions } from "./types/crawler.js";

export const globalOnlyOptions = [
    "maxConnections",
    "rateLimit",
    "priorityLevels",
    "skipDuplicates",
    "homogeneous",
    "userAgents",
    "silence",
];
export const crawlerOnlyOptions = [
    "rateLimiterId",
    "forceUTF8",
    "jQuery",
    "retryInterval",
    "priority",
    "proxy",
    "retries",
    "preRequest",
    "callback",
    "release",
    "isJson",
    "referer",
    "rejectUnauthorized",
    "userParams",
    "autoSelectFamily",
    "autoSelectFamilyAttemptTimeout",
].concat(globalOnlyOptions);
export const deprecatedOptions = [
    "uri",
    "qs",
    "strictSSL",
    "incomingEncoding",
    "gzip",
    "jar",
    "jsonReviver",
    "jsonReplacer",
    "skipEventRequest",
    "logger",
    "debug",
    "time",
    "limiter",
    "gene",
    "jquery",
    "userAgent",
];

export const getCharset = (headers: Record<string, unknown>): null | string => {
    let charset = null;
    const contentType = headers["content-type"] as string;
    if (contentType) {
        const match = contentType.match(/charset=['"]?([\w.-]+)/i);
        if (match) {
            charset = match[1].trim().toLowerCase();
        }
    }
    return charset;
};

export const getValidOptions = (options: RequestConfig): RequestOptions => {
    const type = getType(options);
    if (type === "string") {
        try {
            if (isValidUrl(options as string)) return { url: options } as RequestOptions;
            options = JSON.parse(options as string);
            return options as object;
        } catch (err) {
            throw new TypeError(`Invalid options: ${JSON.stringify(options)}`, { cause: err });
        }
    } else if (type === "object") {
        const prototype = Object.getPrototypeOf(options);
        if (prototype === Object.prototype || prototype === null) return options as object;
    }
    throw new TypeError(`Invalid options: ${JSON.stringify(options)}`);
};

export const renameOptionParams = (options: CrawlerOptions | undefined): CrawlerOptions | undefined => {
    if (options == undefined) {
        return undefined;
    }
    const renamedOptions: CrawlerOptions = {
        ...options,
        url: options.uri ?? options.url,
        searchParams: options.qs ?? options.searchParams,
        rejectUnauthorized: options.strictSSL ?? options.rejectUnauthorized,
        encoding: options.incomingEncoding ?? options.encoding,
        decompress: options.gzip ?? options.decompress,
        cookieJar: options.jar ?? options.cookieJar,
        parseJson: options.jsonReviver ?? options.parseJson,
        stringifyJson: options.jsonReplacer ?? options.stringifyJson,
        rateLimit: options.limiter ?? options.rateLimit,
        userParams: options.gene ?? options.userParams,
        jQuery: options.jquery ?? options.JQuery ?? options.jQuery,
    };
    // Drop keys that resolved to undefined so they don't clobber crawler
    // defaults when spread via { ...defaultOptions, ...options }.
    (Object.keys(renamedOptions) as (keyof CrawlerOptions)[]).forEach(key => {
        if (renamedOptions[key] === undefined) {
            delete renamedOptions[key];
        }
    });
    return renamedOptions;
};

export const alignOptions = (options: RequestOptions): OptionsInit => {
    // "Happy Eyeballs" tuning. got strips unknown options and manages the
    // http2 session internally, so these socket-level settings can't be passed
    // per-request through got — Node only exposes them as process-wide defaults.
    // Applied here (before they are stripped below) so they take effect for both
    // the http1 and http2 paths. Guarded because the setters only exist on Node >=19.
    if (options.autoSelectFamily !== undefined && typeof net.setDefaultAutoSelectFamily === "function") {
        net.setDefaultAutoSelectFamily(options.autoSelectFamily);
    }
    if (options.autoSelectFamilyAttemptTimeout !== undefined && typeof net.setDefaultAutoSelectFamilyAttemptTimeout === "function") {
        net.setDefaultAutoSelectFamilyAttemptTimeout(options.autoSelectFamilyAttemptTimeout);
    }

    const gotOptions = {
        ...options,
        timeout: { request: options.timeout },
    } as any;

    const sslConfig = options.rejectUnauthorized;
    if (sslConfig !== undefined) {
        if (gotOptions.https === undefined) {
            gotOptions.https = { rejectUnauthorized: sslConfig };
        }
        else {
            gotOptions.https.rejectUnauthorized = sslConfig;
        }
    }

    const defaultagent = options["proxy"] ? {
        https: new HttpsProxyAgent({ proxy: options["proxy"] }),
        http: new HttpProxyAgent({ proxy: options["proxy"] }),
    } : undefined;

    // http2 proxy
    if (options.http2 === true && options.proxy) {
        const { proxies: Http2Proxies } = http2Wrapper;
        const protocol = options.proxy.startsWith("https") ? "https" : "http";
        const http2Agent =
            protocol === "https"
                ? new Http2Proxies.Http2OverHttps({
                    proxyOptions: { url: options.proxy },
                })
                : new Http2Proxies.Http2OverHttp({
                    proxyOptions: { url: options.proxy },
                });
        gotOptions.agent = { http2: http2Agent };
    } else {
        gotOptions.agent = gotOptions.agent ?? (options.proxy ? defaultagent : undefined);
    }

    /**
     * @deprecated The support of incomingEncoding will be removed in the next major version.
     */
    gotOptions.responseType = "buffer";

    const invalidOptions = crawlerOnlyOptions.concat(deprecatedOptions);
    invalidOptions.forEach(key => {
        if (key in gotOptions) {
            delete gotOptions[key];
        }
    });

    const headers = gotOptions.headers;
    cleanObject(gotOptions);
    gotOptions.headers = headers;

    if (!gotOptions.headers.referer) {
        if (options.referer) {
            gotOptions.headers.referer = options.referer;
        }
        else {
            const domain = gotOptions.url.match(/^(\w+):\/\/([^/]+)/);
            if (domain) gotOptions.headers.referer = domain[0];
        }
    }

    gotOptions.retry = { limit: 0 };
    return gotOptions;
};
