(() => {
  document.documentElement.classList.add("sim-theme-doc");

  const ready = (fn) => {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn, { once: true });
    } else {
      fn();
    }
  };

  const ignoredTags = new Set(["SCRIPT", "STYLE", "LINK", "TEMPLATE"]);

  function isPageChrome(el) {
    if (!el || ignoredTags.has(el.tagName)) return true;
    if (el.tagName === "FOOTER") return true;
    if (el.tagName === "HEADER") return true;
    if (el.classList && el.classList.contains("topline")) return true;
    return false;
  }

  function hasNativeTheoryTabs() {
    return Boolean(
      document.querySelector("#theory-view, #tab-theory, [data-tab='teoria'], [data-tab='theory']") ||
      Array.from(document.querySelectorAll("button, a")).some((el) => {
        const text = (el.textContent || "").trim().toLowerCase();
        const onclick = (el.getAttribute("onclick") || "").toLowerCase();
        return text === "teoria" || text.includes(" teoria") || onclick.includes("theory") || onclick.includes("teoria");
      })
    );
  }

  function eligibleTheoryNode(node) {
    if (!node || node.closest(".sim-theme-panel")) return false;
    if (node.id === "theory-view" || node.id === "tab-theory") return false;
    if (node.closest(".tab-panel, .tab-page, [role='tabpanel']")) return false;
    return true;
  }

  function normalizeLabel(text) {
    return (text || "")
      .toLowerCase()
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .replace(/[^a-z]/g, "");
  }

  function renameTheoryLabels() {
    Array.from(document.querySelectorAll("button, a")).forEach((el) => {
      const normalized = normalizeLabel(el.textContent);
      if (normalized === "teoria") {
        el.textContent = "Modello";
      }
    });
  }

  function compactTheoryRoot(root, options = {}) {
    if (!root || root.dataset.simThemeCompact === "done") return;
    if (options.skipIfNestedDone && root.querySelector("[data-sim-theme-compact='done'], .sim-theme-model-note")) return;

    const text = (root.textContent || "").replace(/\s+/g, " ").trim();
    if (!options.force && text.length < 900 && !root.querySelector(".theory-block, .theory-card, .th-section")) return;

    root.dataset.simThemeCompact = "done";
    root.classList.add("sim-theme-compact-theory");

    let heading = root.querySelector(":scope > h1, :scope > h2, .theory-head h2, .th-head h2");
    if (heading) {
      heading.textContent = "Modello essenziale";
      heading.classList.add("sim-theme-compact-heading");
    } else if (options.force) {
      heading = document.createElement("h2");
      heading.textContent = "Modello essenziale";
      heading.className = "sim-theme-compact-heading";
      root.prepend(heading);
    }

    const note = document.createElement("p");
    note.className = "sim-theme-model-note";
    note.innerHTML = "<strong>In breve.</strong> Questa scheda tiene solo le idee operative per leggere la simulazione: modello usato, grandezze da osservare, formule principali e limiti dell'esperimento digitale. La trattazione completa sta meglio nel libro teorico.";
    if (heading && heading.parentNode) {
      heading.insertAdjacentElement("afterend", note);
    } else {
      root.prepend(note);
    }

    const directBlocks = Array.from(root.querySelectorAll(
      ":scope > .theory-block, :scope > .theory-card, :scope > .th-section, :scope > article, :scope > section"
    ));
    if (directBlocks.length > 3) {
      directBlocks.slice(3).forEach((el) => el.classList.add("sim-theme-theory-extra"));
      return;
    }

    const nestedBlocks = Array.from(root.querySelectorAll(".theory-block, .theory-card, .th-section"));
    if (nestedBlocks.length > 3) {
      nestedBlocks.slice(3).forEach((el) => el.classList.add("sim-theme-theory-extra"));
      return;
    }

    const contentChildren = Array.from(root.children).filter((el) => {
      if (el.classList.contains("sim-theme-model-note")) return false;
      if (/^H[1-6]$/.test(el.tagName)) return false;
      if (["SCRIPT", "STYLE", "LINK", "TEMPLATE", "NAV"].includes(el.tagName)) return false;
      return true;
    });
    if (contentChildren.length > 8) {
      contentChildren.slice(8).forEach((el) => el.classList.add("sim-theme-theory-extra"));
    }
  }

  function compactTheoryRoots() {
    const roots = Array.from(document.querySelectorAll(
      ".theory-section, section.theory, div.theory, #theory-view, #tab-theory"
    ));
    roots.forEach(compactTheoryRoot);
  }

  function buildTabs() {
    document.body.classList.add("sim-theme");
    renameTheoryLabels();

    if (hasNativeTheoryTabs()) {
      document.body.classList.add("sim-theme-native-tabs");
      compactTheoryRoots();
      return;
    }

    const baseTheoryNodes = Array.from(
      document.querySelectorAll(".theory-section, section.theory, div.theory")
    ).filter(eligibleTheoryNode);

    if (!baseTheoryNodes.length) return;

    const parent = baseTheoryNodes[0].parentElement;
    if (!parent) return;

    const directTheoryNodes = baseTheoryNodes.filter((node) => node.parentElement === parent);
    if (!directTheoryNodes.length) return;

    const children = Array.from(parent.children);
    const firstTheoryIndex = children.indexOf(directTheoryNodes[0]);
    if (firstTheoryIndex < 0) return;

    const extraIntroNodes = Array.from(parent.querySelectorAll(":scope > .intro-block"))
      .filter((node) => eligibleTheoryNode(node) && children.indexOf(node) > -1 && children.indexOf(node) < firstTheoryIndex);

    const theorySet = new Set([...extraIntroNodes, ...directTheoryNodes]);

    let firstContent = children.find((node) => !isPageChrome(node));
    if (!firstContent) firstContent = directTheoryNodes[0];

    const shell = document.createElement("div");
    shell.className = "sim-theme-shell";

    const nav = document.createElement("nav");
    nav.className = "sim-theme-tabs";
    nav.setAttribute("aria-label", "Vista");

    const simButton = document.createElement("button");
    simButton.type = "button";
    simButton.className = "sim-theme-tab";
    simButton.textContent = "Simulazione";
    simButton.setAttribute("aria-controls", "sim-theme-sim");

    const theoryButton = document.createElement("button");
    theoryButton.type = "button";
    theoryButton.className = "sim-theme-tab";
    theoryButton.textContent = "Modello";
    theoryButton.setAttribute("aria-controls", "sim-theme-theory");

    nav.append(simButton, theoryButton);

    const simPanel = document.createElement("div");
    simPanel.className = "sim-theme-panel sim-theme-panel--sim";
    simPanel.id = "sim-theme-sim";

    const theoryPanel = document.createElement("div");
    theoryPanel.className = "sim-theme-panel sim-theme-panel--theory";
    theoryPanel.id = "sim-theme-theory";

    shell.append(nav, simPanel, theoryPanel);
    parent.insertBefore(shell, firstContent);

    Array.from(parent.children).forEach((child) => {
      if (child === shell || isPageChrome(child)) return;
      if (theorySet.has(child)) {
        theoryPanel.appendChild(child);
      } else {
        simPanel.appendChild(child);
      }
    });

    const storageKey = `sim-theme-view:${window.location.pathname}`;
    const setView = (view) => {
      const isTheory = view === "theory";
      simPanel.hidden = isTheory;
      theoryPanel.hidden = !isTheory;
      simButton.classList.toggle("is-active", !isTheory);
      theoryButton.classList.toggle("is-active", isTheory);
      simButton.setAttribute("aria-selected", String(!isTheory));
      theoryButton.setAttribute("aria-selected", String(isTheory));
      document.body.dataset.simThemeView = view;
      try { window.localStorage.setItem(storageKey, view); } catch (_) {}
      requestAnimationFrame(() => window.dispatchEvent(new Event("resize")));
    };

    simButton.addEventListener("click", () => setView("sim"));
    theoryButton.addEventListener("click", () => setView("theory"));

    let initial = "sim";
    try {
      const stored = window.localStorage.getItem(storageKey);
      if (stored === "theory") initial = "theory";
    } catch (_) {}
    setView(initial);
    compactTheoryRoots();
    compactTheoryRoot(theoryPanel, { force: true, skipIfNestedDone: true });
  }

  ready(buildTabs);
})();
