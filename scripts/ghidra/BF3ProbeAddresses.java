// Report what the current program has at each address listed in a file (one address per line,
// overlay addresses as <space>::<addr>): instruction, data, containing function, bookmarks.
// Also prints a count of Error/Warning bookmarks by category. Read-only.
// Arguments: <addresses.txt> [max lines to report, default 40]
//@category BF3
//@author bf3-recomp

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class BF3ProbeAddresses extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		List<String> lines = Files.readAllLines(Path.of(args[0]));
		int max = args.length > 1 ? Integer.parseInt(args[1]) : 40;
		Map<String, Integer> states = new TreeMap<>();
		int shown = 0;
		for (String line : lines) {
			line = line.trim();
			if (line.isEmpty()) {
				continue;
			}
			Address a = currentProgram.getAddressFactory().getAddress(line);
			Instruction insn = getInstructionAt(a);
			Data data = getDataAt(a);
			Function containing = getFunctionContaining(a);
			Data inside = data == null ? getDataContaining(a) : null;
			String state = insn != null ? (containing != null ? "insn-in-function" : "insn-no-function")
					: data != null ? (data.isDefined() ? "data:" + data.getDataType().getName() : "undefined")
							: inside != null ? "inside-data:" + inside.getDataType().getName()
									: currentProgram.getListing().isUndefined(a, a) ? "undefined-bytes"
											: "nothing";
			states.merge(state, 1, Integer::sum);
			if (shown++ < max) {
				StringBuilder bm = new StringBuilder();
				for (Bookmark b : currentProgram.getBookmarkManager().getBookmarks(a)) {
					bm.append(" [").append(b.getTypeString()).append('/').append(b.getCategory())
							.append(": ").append(b.getComment()).append(']');
				}
				println(String.format("PROBE %s %s%s%s%s", a, state,
					inside != null ? String.format(" (%s, %d bytes)", inside.getMinAddress(), inside.getLength())
							: "",
					containing != null ? " in " + containing.getName() : "", bm));
			}
		}
		states.forEach((k, v) -> println(String.format("STATE %-24s %d", k, v)));

		Map<String, Integer> bookmarks = new TreeMap<>();
		for (String type : new String[] { BookmarkType.ERROR, BookmarkType.WARNING }) {
			Iterator<Bookmark> it = currentProgram.getBookmarkManager().getBookmarksIterator(type);
			while (it.hasNext()) {
				Bookmark b = it.next();
				String comment = b.getComment().replaceAll("[0-9a-fA-F]{8}", "ADDR");
				bookmarks.merge(type + "/" + b.getCategory() + ": " + comment, 1, Integer::sum);
			}
		}
		bookmarks.forEach((k, v) -> println(String.format("BOOKMARK %6d %s", v, k)));
	}
}
