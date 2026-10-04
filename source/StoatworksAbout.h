/*
 * Stoatworks Labs - About window data for Lowband.
 *
 * PROVISIONAL HAND COPY, written 2026-10-04 in the shape that
 * stoatworks-backend/scripts/sync-about.py generates. Lowband is not yet in
 * the website's projects.json, so there is nothing to generate it from; the
 * first sync after registration overwrites this file. There is no user guide
 * yet, so `guide` is empty and the block has no "User guide" button.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Lowband";
    inline constexpr auto slug = "lowband";
    inline constexpr auto hook = "A Hi8 tape on a Video8 deck, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/lowband/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/lowband";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
