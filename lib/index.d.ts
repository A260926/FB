/// <reference types="cheerio" />

declare namespace scrapeIt {
    export interface ScrapeOptions {
        [key: string]: string | ScrapeOptionList | ScrapeOptionElement;
    }

    /**
     * Linked extraction: instead of returning the link attribute, the field
     * resolves to the data extracted from the linked page.
     */
    export interface ScrapeOptionFollow {
        /**
         * The schema applied to the detail page. It may contain further
         * `follow` fields at any depth.
         */
        data: ScrapeOptions;
    }

    export interface ScrapeOptionElement {
        selector?: string;
        convert?: (value: any) => any;
        // using Cheerio type directly from modules it won't show warning types.
        how?: string | ((element: Cheerio) => any);
        attr?: string;
        trim?: boolean;
        trimValue?: boolean;
        closest?: string;
        eq?: number;
        texteq?: number;
        data?: ScrapeOptions;
        /**
         * When provided, the field fetches the selected link with a new GET
         * request and extracts the linked page using `follow.data`.
         * Cannot be combined with `how` or `texteq`.
         */
        follow?: ScrapeOptionFollow;
    }

    export interface ScrapeOptionList {
        listItem: string;
        data?: ScrapeOptions;
        convert?: (value: any) => any;
    }

    export interface ScrapeResult<T> {
        data: T,
        status: number,
        statusText: string,
        $: Cheerio,
        body: string
    }

    /**
     * Synchronous scraping. Throws an error with code `ASYNC_FOLLOW_REQUIRED`
     * when `options` contains any `follow` field: linked extraction requires
     * the async `scrapeIt(url, options)` entry point and never starts requests
     * from here.
     */
    export function scrapeHTML<T>(body: Cheerio | string, options: ScrapeOptions): T;
}

declare function scrapeIt<T>(url: string | object, opts: scrapeIt.ScrapeOptions): Promise<scrapeIt.ScrapeResult<T>>;

export = scrapeIt;
