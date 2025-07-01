module.exports = function(app, ptyProcess) {
  app.use(require('express').json());
  app.post('/api/command', async (req, res) => {
    const { command } = req.body;
    if (!command || typeof command !== "string") {
      return res.status(400).json({ error: "Missing command" });
    }

    let outputBuffer = "";
    let responded = false;
    let inactivityTimer;

    function resetInactivityTimer() {
      clearTimeout(inactivityTimer);
      inactivityTimer = setTimeout(finish, 500); // 2s inactivity
    }

    function onData(data) {
      outputBuffer += data;
      resetInactivityTimer();
    }

    function finish() {
      if (responded) return;
      responded = true;
      clearTimeout(inactivityTimer);
      clearTimeout(hardTimeout);
      ptyProcess.removeListener("data", onData);

      console.log(outputBuffer);

      // Improved prompt pattern: matches common shell prompts at line end, including user@host:path$ or #, with optional spaces
      const promptPattern = /^\s*[\w.@~☁:/\-\[\]{}()=\$%# ]+[#$%]\s*$/m;
      let cleaned = outputBuffer
        .replace(/\x1B\[[0-9;]*[a-zA-Z]/g, "") // remove ANSI
        .replace(/\r/g, "")
        .replace(/[\b]/g, "")
        .split("\n")
        // remove first \n
        .slice(2,-1)
        .map(line => line.trim())
        .filter(line => line.length > 0)
        .filter(line => line !== command) // remove echoed command
        .filter(line => !promptPattern.test(line)) // remove prompt lines
        .join("\n");

    //   cleaned = "test";
      res.json({ output: cleaned });
    }

    // Hard timeout fallback
    const hardTimeout = setTimeout(() => {
      if (!responded) {
        responded = true;
        ptyProcess.removeListener("data", onData);
        res.status(504).json({ error: "Command timed out" });
      }
    }, 10000);

    ptyProcess.on("data", onData);
    ptyProcess.write(`${command}\n`);
    resetInactivityTimer();
  });
};