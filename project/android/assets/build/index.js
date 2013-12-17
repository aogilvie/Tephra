var ctx =canvas.getContext("2d");

var img = new Image();

window.pos_x = 0;
window.pos_y = 0;

img.src = 'bg.png';

var w = window.innerWidth;
var h = window.innerHeight;
var w2 = w/2;
var h2 = h/2;

var curves = [];
for( var i = 0; i < 200; i++ ) {
	curves.push({
		current: Math.random() * 1000,
		inc: Math.random() * 0.005 + 0.002,
		color: '#'+(Math.random()*0xFFFFFF<<0).toString(16) // Random color
	});
}

var p = [0,0, 0,0, 0,0, 0,0];
var animate = function() {
	// Clear the screen - note that .globalAlpha is still honored,
	// so this will only "darken" the screen a bit
	
	ctx.globalCompositeOperation = 'source-over';
	ctx.fillRect(0,0,w,h);

	// Use the additive blend mode to draw the bezier curves
	ctx.globalCompositeOperation = 'lighter';

	// Calculate curve positions and draw
	for( var i = 0; i < maxCurves; i++ ) {
		var curve = curves[i];
		curve.current += curve.inc;
		for( var j = 0; j < p.length; j+=2 ) {
			var a = Math.sin( curve.current * (j+3) * 373 * 0.0001 );
			var b = Math.sin( curve.current * (j+5) * 927 * 0.0002 );
			var c = Math.sin( curve.current * (j+5) * 573 * 0.0001 );
			p[j] = (a * a * b + c * a + b) * w * c + w2;
			p[j+1] = (a * b * b + c - a * b *c) * h2 + h2;
		}

		ctx.beginPath();
		ctx.moveTo( p[0], p[1] );
		ctx.bezierCurveTo( p[2], p[3], p[4], p[5], p[6], p[7] );
		ctx.strokeStyle = curve.color;
		ctx.stroke();
	}
	if(window.pos_x>200)window.pos_x=0;
	if(window.pos_y>200)window.pos_y=0;
	window.pos_x++;
	window.pos_y++;

	ctx.save();

	ctx.globalAlpha = 1;
	if(img)ctx.drawImage(img, window.pos_x, window.pos_y);

	ctx.restore();

};


// The vertical touch position controls the number of curves;
// horizontal controls the line width
var maxCurves = 70;
document.addEventListener( 'touchmove', function( ev ) {
	ctx.lineWidth = (ev.touches[0].pageX/w) * 20;
	maxCurves = Math.floor((ev.touches[0].pageY/h) * curves.length);
}, false );


ctx.fillStyle = '#000000';
ctx.fillRect( 0, 0, w, h );

ctx.globalAlpha = 0.5;
ctx.lineWidth = 2;
setInterval( animate, 16 );

//test XMLHttpRequest
var request = new XMLHttpRequest()
request.open("GET","www.google.com");
request.send()
