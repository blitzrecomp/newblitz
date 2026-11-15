/* New Blitz 2027 site: the only script. No tracking, no third parties.
   1. The trailer: the YouTube player loads (from youtube-nocookie.com) only when someone presses play.
   2. Screenshots: until tools/build.py has baked assets/shots/shots.json into the pages (<meta name="nb-shots"
      content="baked">), read it here and fill the slots, so dropping in shots.json works even before a build. */
(function () {
  "use strict";

  document.addEventListener("click", function (e) {
    var a = e.target.closest && e.target.closest(".trailer[data-yt] .trailer-play");
    if (!a) return;
    e.preventDefault();
    var box = a.parentNode, id = box.getAttribute("data-yt");
    var f = document.createElement("iframe");
    f.src = "https://www.youtube-nocookie.com/embed/" + encodeURIComponent(id) + "?autoplay=1&rel=0&modestbranding=1";
    f.title = "New Blitz 2027 trailer";
    f.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen";
    f.allowFullscreen = true;
    f.referrerPolicy = "strict-origin-when-cross-origin";
    box.replaceChildren(f);
    f.focus();
  });

  // Copy buttons (the contributor page's agent prompts): <button class="copy-btn" data-copy="<pre id>">.
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest(".copy-btn[data-copy]");
    if (!b) return;
    var src = document.getElementById(b.getAttribute("data-copy"));
    if (!src || !navigator.clipboard) return;
    navigator.clipboard.writeText(src.innerText.replace(/\s+$/, "") + "\n").then(function () {
      var was = b.textContent;
      b.textContent = "Copied";
      b.classList.add("is-copied");
      setTimeout(function () { b.textContent = was; b.classList.remove("is-copied"); }, 1600);
    }).catch(function () {});
  });

  var meta = document.querySelector('meta[name="nb-shots"]');
  if (!meta || meta.content !== "pending") return;
  var slots = document.querySelectorAll("figure[data-shot]");
  var gallery = document.querySelector("[data-gallery]");
  if (!slots.length && !gallery) return;

  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function stem(f) { return f.split("/").pop().replace(/\.[^.]+$/, ""); }
  function normalise(data) {
    if (data && !Array.isArray(data)) {
      data = data.shots || Object.keys(data).map(function (k) {
        var v = data[k]; return typeof v === "object" ? Object.assign({ slot: k }, v) : { slot: k, file: v };
      });
    }
    return (data || []).filter(function (e) { return e && e.file; }).map(function (e) {
      var s = e.slots || e.slot || e.name || e.id || stem(e.file);
      var full = e.file.charAt(0) === "/" ? e.file : "/assets/shots/" + e.file.replace(/^\.?\//, "");
      return { slots: [].concat(s), full: full, caption: e.caption || "", alt: e.what || e.alt || e.shows || e.caption || "" };
    });
  }
  function inner(s, label) {
    return '<a class="shot-frame" href="' + esc(s.full) + '"><img src="' + esc(s.full) + '" alt="' + esc(s.alt) +
      '" loading="lazy" decoding="async"></a><figcaption>' + esc(s.caption || label) + "</figcaption>";
  }

  fetch("/assets/shots/shots.json", { cache: "no-cache" }).then(function (r) {
    return r.ok ? r.json() : null;
  }).then(function (data) {
    if (!data) return;
    var shots = normalise(data), by = {};
    shots.forEach(function (s) { s.slots.forEach(function (k) { if (!by[k]) by[k] = s; }); });
    Array.prototype.forEach.call(slots, function (fig) {
      var s = by[fig.getAttribute("data-shot")];
      if (!s) return;
      fig.innerHTML = inner(s, fig.getAttribute("data-label") || "");
      fig.classList.add("has-shot");
    });
    if (gallery && shots.length) {
      gallery.innerHTML = '<div class="gallery">' + shots.map(function (s) {
        return '<figure class="shot has-shot">' + inner(s, "") + "</figure>";
      }).join("") + "</div>";
    }
  }).catch(function () {});
})();
