// Add PC-98 names to in/out ports and interrupts. Arg "com" (as pre-script) only creates the entry at CS:0100.
// @category Rusty

import java.util.HashMap;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.SourceType;

public class Pc98Annotate extends GhidraScript {

	private static final Map<Integer, String> PORTS = new HashMap<>();
	private static final Map<Integer, String> INTERRUPTS = new HashMap<>();
	private static final Map<Integer, String> DOS = new HashMap<>();
	private static final Map<Integer, String> BIOS18 = new HashMap<>();

	static {
		PORTS.put(0x00, "PIC master cmd");
		PORTS.put(0x02, "PIC master mask");
		PORTS.put(0x08, "PIC slave cmd");
		PORTS.put(0x0A, "PIC slave mask");
		PORTS.put(0x31, "system port A (DIP sw)");
		PORTS.put(0x33, "system port B");
		PORTS.put(0x35, "system port C (beep, RS-232C mask)");
		PORTS.put(0x37, "system port ctrl");
		PORTS.put(0x41, "keyboard data");
		PORTS.put(0x43, "keyboard ctrl/status");
		PORTS.put(0x60, "GDC text: status / param");
		PORTS.put(0x62, "GDC text: cmd / data read");
		PORTS.put(0x64, "CRTV vsync interrupt reset");
		PORTS.put(0x68, "mode flip-flop 1 (kanji access, 400/200 line)");
		PORTS.put(0x6A, "mode flip-flop 2 (analog palette, EGC, GDC clock)");
		PORTS.put(0x6C, "border color");
		PORTS.put(0x71, "PIT counter 0 (timer)");
		PORTS.put(0x73, "PIT counter 1 (beep)");
		PORTS.put(0x75, "PIT counter 2 (RS-232C)");
		PORTS.put(0x77, "PIT ctrl");
		PORTS.put(0x7C, "GRCG mode");
		PORTS.put(0x7E, "GRCG tile");
		PORTS.put(0xA0, "GDC graphic: status / param");
		PORTS.put(0xA1, "kanji code 2nd byte");
		PORTS.put(0xA2, "GDC graphic: cmd / data read");
		PORTS.put(0xA3, "kanji code 1st byte");
		PORTS.put(0xA4, "graphic display page");
		PORTS.put(0xA5, "kanji line (L/R)");
		PORTS.put(0xA6, "graphic draw page");
		PORTS.put(0xA8, "palette index / digital palette 3,7");
		PORTS.put(0xA9, "kanji data (CG RAM)");
		PORTS.put(0xAA, "palette green / digital palette 2,6");
		PORTS.put(0xAC, "palette red / digital palette 1,5");
		PORTS.put(0xAE, "palette blue / digital palette 0,4");
		PORTS.put(0x188, "OPN/OPNA address (port 1)");
		PORTS.put(0x18A, "OPN/OPNA data (port 1)");
		PORTS.put(0x18C, "OPNA address (port 2)");
		PORTS.put(0x18E, "OPNA data (port 2)");
		PORTS.put(0xA460, "sound board ID / OPNA mask (86 board)");
		PORTS.put(0xA468, "86 PCM FIFO ctrl");
		PORTS.put(0xA46C, "86 PCM FIFO data");
		PORTS.put(0x4A0, "EGC plane access");
		PORTS.put(0x4A2, "EGC pattern / read plane");
		PORTS.put(0x4A4, "EGC ROP / mode");
		PORTS.put(0x4A6, "EGC foreground color");
		PORTS.put(0x4A8, "EGC mask");
		PORTS.put(0x4AA, "EGC background color");
		PORTS.put(0x4AC, "EGC bit address / shift");
		PORTS.put(0x4AE, "EGC bit length");

		INTERRUPTS.put(0x08, "IRQ0 timer");
		INTERRUPTS.put(0x0A, "IRQ2 vsync");
		INTERRUPTS.put(0x18, "BIOS: keyboard / CRT / graphics");
		INTERRUPTS.put(0x1B, "BIOS: disk");
		INTERRUPTS.put(0x1C, "BIOS: timer / calendar");
		INTERRUPTS.put(0x1F, "BIOS: misc");
		INTERRUPTS.put(0x20, "DOS: terminate");
		INTERRUPTS.put(0x21, "DOS");
		INTERRUPTS.put(0x60, "PMD music driver");
		INTERRUPTS.put(0x67, "EMS");

		DOS.put(0x09, "print string");
		DOS.put(0x0E, "select drive");
		DOS.put(0x19, "get drive");
		DOS.put(0x1A, "set DTA");
		DOS.put(0x25, "set vector");
		DOS.put(0x30, "get version");
		DOS.put(0x31, "keep resident");
		DOS.put(0x35, "get vector");
		DOS.put(0x3B, "chdir");
		DOS.put(0x3C, "create");
		DOS.put(0x3D, "open");
		DOS.put(0x3E, "close");
		DOS.put(0x3F, "read");
		DOS.put(0x40, "write");
		DOS.put(0x41, "delete");
		DOS.put(0x42, "seek");
		DOS.put(0x44, "ioctl");
		DOS.put(0x47, "get cwd");
		DOS.put(0x48, "alloc");
		DOS.put(0x49, "free");
		DOS.put(0x4A, "resize");
		DOS.put(0x4B, "exec");
		DOS.put(0x4C, "exit");
		DOS.put(0x4E, "find first");
		DOS.put(0x4F, "find next");

		BIOS18.put(0x00, "read key");
		BIOS18.put(0x01, "key buffer status");
		BIOS18.put(0x04, "key group state");
		BIOS18.put(0x0A, "set text mode");
		BIOS18.put(0x0C, "text display on");
		BIOS18.put(0x0D, "text display off");
		BIOS18.put(0x10, "cursor blink");
		BIOS18.put(0x11, "cursor on");
		BIOS18.put(0x12, "cursor off");
		BIOS18.put(0x16, "clear text VRAM");
		BIOS18.put(0x40, "graphic display on");
		BIOS18.put(0x41, "graphic display off");
		BIOS18.put(0x42, "set graphic mode");
		BIOS18.put(0x4D, "GDC 5/2.5 MHz");
	}

	private Integer registerValueBefore(Instruction instruction, String register, int maxSteps) {
		Instruction previous = instruction.getPrevious();
		for (int step = 0; step < maxSteps && previous != null; step++) {
			String mnemonic = previous.getMnemonicString();
			if (previous.getNumOperands() == 2 && previous.getDefaultOperandRepresentation(0).equalsIgnoreCase(register)) {
				Scalar scalar = previous.getScalar(1);
				if (mnemonic.equalsIgnoreCase("MOV") && scalar != null) {
					return (int) scalar.getUnsignedValue();
				}
				return null;
			}
			if (previous.getFlowType().isJump() || previous.getFlowType().isCall()) {
				return null;
			}
			previous = previous.getPrevious();
		}
		return null;
	}

	private Integer portOf(Instruction instruction) {
		String mnemonic = instruction.getMnemonicString().toUpperCase();
		int operand = mnemonic.equals("IN") ? 1 : 0;
		Scalar scalar = instruction.getScalar(operand);
		if (scalar != null) {
			return (int) scalar.getUnsignedValue();
		}
		return registerValueBefore(instruction, "DX", 8);
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length > 0 && args[0].equals("com")) {
			Address entry = currentProgram.getMinAddress();
			disassemble(entry);
			if (getFunctionAt(entry) == null) {
				createFunction(entry, "com_entry");
			}
			addEntryPoint(entry);
			return;
		}

		int ports = 0;
		int interrupts = 0;
		InstructionIterator iterator = currentProgram.getListing().getInstructions(true);
		while (iterator.hasNext() && !monitor.isCancelled()) {
			Instruction instruction = iterator.next();
			String mnemonic = instruction.getMnemonicString().toUpperCase();
			String comment = null;
			if (mnemonic.equals("IN") || mnemonic.equals("OUT")) {
				Integer port = portOf(instruction);
				if (port != null) {
					String name = PORTS.getOrDefault(port, "unknown port");
					comment = String.format("port %Xh: %s", port, name);
					ports++;
				}
			}
			else if (mnemonic.equals("INT")) {
				Scalar scalar = instruction.getScalar(0);
				if (scalar != null) {
					int number = (int) scalar.getUnsignedValue();
					comment = String.format("INT %02Xh: %s", number, INTERRUPTS.getOrDefault(number, "?"));
					Integer function = registerValueBefore(instruction, "AH", 8);
					if (function == null) {
						Integer ax = registerValueBefore(instruction, "AX", 8);
						function = ax == null ? null : ax >> 8;
					}
					if (function != null) {
						Map<Integer, String> table = number == 0x21 ? DOS : number == 0x18 ? BIOS18 : null;
						String name = table == null ? null : table.get(function);
						comment += String.format(" AH=%02Xh%s", function, name == null ? "" : " " + name);
					}
					interrupts++;
				}
			}
			if (comment != null && getEOLComment(instruction.getAddress()) == null) {
				setEOLComment(instruction.getAddress(), comment);
			}
		}
		println(String.format("%s: %d port and %d interrupt comments", currentProgram.getName(), ports, interrupts));
	}
}
