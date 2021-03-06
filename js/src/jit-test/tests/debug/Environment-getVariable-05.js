// getVariable sees global variables.

var g = newGlobal();
var dbg = Debugger(g);
var log = '';
dbg.onDebuggerStatement = function (frame) {
    log += frame.environment.parent.parent.getVariable("x") + frame.environment.parent.parent.getVariable("y");
};
g.eval("var x = 'a'; this.y = 'b'; debugger;");
assertEq(log, 'ab');
