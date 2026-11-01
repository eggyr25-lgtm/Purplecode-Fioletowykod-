if (typeof document !== "undefined") {
  const button = document.querySelector("#go");
  const output = document.querySelector("#out");
  button?.addEventListener("click", () => { output.textContent = `JavaScript executed at ${new Date().toLocaleTimeString()}`; });
} else {
  console.log("Purplecode JS engine test: executed in Node.js");
}
