// Keep diagram labels readable; the surrounding region provides scrolling.
const initializeDiagram = window.mermaid.initialize.bind(window.mermaid);
window.mermaid.initialize = options => initializeDiagram({
  ...options,
  flowchart: { ...options.flowchart, useMaxWidth: false },
  sequence: { ...options.sequence, useMaxWidth: false }
});
