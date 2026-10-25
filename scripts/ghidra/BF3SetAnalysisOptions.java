// Set auto-analysis options on the current program, for use as a headless -preScript.
// Arguments: one or more "<Option Name>=<value>" (e.g. "Non-Returning Functions - Discovered=false").
//@category BF3
//@author bf3-recomp

import ghidra.app.script.GhidraScript;

public class BF3SetAnalysisOptions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		for (String arg : getScriptArgs()) {
			int eq = arg.lastIndexOf('=');
			if (eq <= 0) {
				throw new IllegalArgumentException("expected <Option Name>=<value>: " + arg);
			}
			String name = arg.substring(0, eq), value = arg.substring(eq + 1);
			if (!getCurrentAnalysisOptionsAndValues(currentProgram).containsKey(name)) {
				throw new IllegalArgumentException("no analysis option named '" + name + "'");
			}
			setAnalysisOption(currentProgram, name, value);
			println(String.format("OPTION %s = %s", name, value));
		}
	}
}
