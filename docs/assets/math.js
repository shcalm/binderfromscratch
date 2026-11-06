window.MathJax = {
  tex: { inlineMath: [["\\(", "\\)"]], displayMath: [["\\[", "\\]"]] },
  options: { ignoreHtmlClass: ".*|", processHtmlClass: "arithmatex" }
};
// Formula source stays readable if the optional renderer cannot load.
document$.subscribe(() => {
  if (window.MathJax && window.MathJax.typesetPromise) {
    window.MathJax.typesetPromise();
  }
});
