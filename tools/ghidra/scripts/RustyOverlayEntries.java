// Mark the entry table at offset 0 of a Rusty overlay and create functions at its code targets.
// Args: <table words or "auto"> <code entry indexes, e.g. "0-6" or "2,3,4" or "all"> [import slot indexes]
// @category Rusty

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.WordDataType;
import ghidra.program.model.symbol.SourceType;

public class RustyOverlayEntries extends GhidraScript {

	private List<Integer> parseIndexes(String spec, int count) {
		List<Integer> result = new ArrayList<>();
		if (spec.equals("all")) {
			for (int i = 0; i < count; i++) {
				result.add(i);
			}
			return result;
		}
		if (spec.equals("none")) {
			return result;
		}
		for (String part : spec.split(",")) {
			if (part.contains("-")) {
				String[] range = part.split("-");
				for (int i = Integer.parseInt(range[0]); i <= Integer.parseInt(range[1]); i++) {
					result.add(i);
				}
			}
			else {
				result.add(Integer.parseInt(part));
			}
		}
		return result;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: RustyOverlayEntries <words|auto> <code indexes> [import indexes]");
			return;
		}
		Address base = currentProgram.getMinAddress();
		long size = currentProgram.getMaxAddress().subtract(base) + 1;

		int words;
		if (args[0].equals("auto")) {
			int lowest = Integer.MAX_VALUE;
			int index = 0;
			while (index * 2 < lowest && index * 2 < size) {
				int value = getShort(base.add(index * 2)) & 0xFFFF;
				if (value < lowest) {
					lowest = value;
				}
				index++;
			}
			words = index;
		}
		else {
			words = Integer.parseInt(args[0]);
		}

		List<Integer> code = parseIndexes(args[1], words);
		List<Integer> imports = args.length > 2 ? parseIndexes(args[2], words) : new ArrayList<>();

		createLabel(base, "ovl_entry_table", true, SourceType.USER_DEFINED);
		for (int i = 0; i < words; i++) {
			Address slot = base.add(i * 2);
			clearListing(slot, slot.add(1));
			createData(slot, WordDataType.dataType);
			int target = getShort(slot) & 0xFFFF;
			if (imports.contains(i)) {
				createLabel(slot, String.format("ovl_import_%02d", i), true, SourceType.USER_DEFINED);
				setEOLComment(slot, "filled at run time by the host (far pointer part)");
				continue;
			}
			if (target == 0 || target >= size) {
				continue;
			}
			Address address = base.add(target);
			if (code.contains(i)) {
				String name = String.format("ovl_entry_%02d", i);
				disassemble(address);
				if (getFunctionAt(address) == null) {
					createFunction(address, name);
				}
				else {
					getFunctionAt(address).setName(name, SourceType.USER_DEFINED);
				}
				setEOLComment(slot, "-> " + name);
			}
			else {
				createLabel(address, String.format("ovl_data_%02d", i), true, SourceType.USER_DEFINED);
				setEOLComment(slot, "-> data");
			}
		}
		println(String.format("%s: %d table words, %d code entries", currentProgram.getName(), words, code.size()));
	}
}
