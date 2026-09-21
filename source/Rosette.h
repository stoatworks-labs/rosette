#pragma once

#include "Audio.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
    Rosette -- offset litho, as an FFGL effect for Resolume.

    A printed picture is four halftone plates, each a lattice of dots at its
    own screen angle, laid down one after another by a press that never quite
    registers them. Model the plates and the press, and everything a printer
    recognises falls out rather than being drawn: the rosettes where the four
    lattices interfere, the moire when two are set too close, the colour
    fringes where the plates are off and the way they wander as the press
    runs, the fattening of the midtones as the ink spreads, and the
    overprint arithmetic that makes cyan over yellow green.

    **Two passes**, in Shaders.h. Separation into a mipmapped CMYK buffer,
    then one print pass that screens all four plates and lays the inks down.

    See AGENTS.md for the traps.
*/
class Rosette : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale );

	Rosette();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The value the render actually uses: the active preset's, where it has
	/// one, otherwise the host's. Public because the harness asks.
	float Effective( unsigned int index ) const;

	/// The parameter ids a preset covers, in presets::Param order. Handed out
	/// rather than copied into the harness, so a second list cannot go
	/// quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	/// Where each plate sat on the last rendered frame, in pixels, after
	/// registration, wander and audio. For the harness.
	void PlateOffsetsForTest( float out[ 4 ][ 2 ] ) const;

	/// The analyser, for the harness to count onsets.
	const rosette::audio::Analyser& AnalyserForTest() const
	{
		return analyser;
	}

	/// The order the host shows them in: separate, screen, run the press,
	/// choose the inks, put it back.
	enum ParamID : FFUInt32
	{
		//Separation
		PT_BLACK_GEN,
		PT_TOTAL_INK,

		//Screen
		PT_SCREEN,
		PT_DOT_SHAPE,
		PT_DOT_GAIN,
		PT_INK_SPREAD,
		PT_ANGLE_C,
		PT_ANGLE_M,
		PT_ANGLE_Y,
		PT_ANGLE_K,

		//Press
		PT_REG_C_X,
		PT_REG_C_Y,
		PT_REG_M_X,
		PT_REG_M_Y,
		PT_REG_Y_X,
		PT_REG_Y_Y,
		PT_REG_K_X,
		PT_REG_K_Y,
		PT_WANDER,
		PT_WANDER_SPEED,
		PT_AUDIO,
		PT_AUDIO_DRIVE,

		//Ink
		PT_INK_C_R,
		PT_INK_C_G,
		PT_INK_C_B,
		PT_INK_M_R,
		PT_INK_M_G,
		PT_INK_M_B,
		PT_INK_Y_R,
		PT_INK_Y_G,
		PT_INK_Y_B,
		PT_INK_K_R,
		PT_INK_K_G,
		PT_INK_K_B,
		PT_PAPER_R,
		PT_PAPER_G,
		PT_PAPER_B,
		PT_INK_DENSITY,
		PT_PLATE_C,
		PT_PLATE_M,
		PT_PLATE_Y,
		PT_PLATE_K,
		PT_SOLO,

		//Output
		PT_MIX,

		//Preset
		PT_PRESET,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The ParamID each presets::Param drives, in presets::Param order.
	static constexpr unsigned int kPresetParamIDs[ rosette::presets::kParamCount ] = {
		PT_BLACK_GEN, PT_TOTAL_INK, PT_SCREEN, PT_DOT_SHAPE, PT_DOT_GAIN, PT_INK_SPREAD,
		PT_ANGLE_C, PT_ANGLE_M, PT_ANGLE_Y, PT_ANGLE_K, PT_WANDER, PT_WANDER_SPEED,
		PT_INK_C_R, PT_INK_C_G, PT_INK_C_B, PT_INK_M_R, PT_INK_M_G, PT_INK_M_B,
		PT_INK_Y_R, PT_INK_Y_G, PT_INK_Y_B, PT_INK_K_R, PT_INK_K_G, PT_INK_K_B,
		PT_PAPER_R, PT_PAPER_G, PT_PAPER_B, PT_INK_DENSITY,
		PT_PLATE_C, PT_PLATE_M, PT_PLATE_Y, PT_PLATE_K
	};

	/// True when this write is the HOST restating a value it still believes
	/// in rather than the operator moving anything.
	bool hostIsRestatingItself( unsigned int index, float value );
	void seedHostValues();
	bool presetCovers( unsigned int index ) const;

	/// What the HOST last sent for each parameter. Resolume owns parameter
	/// state and pushes its own values back down whenever it likes; while a
	/// preset is active those restatements must not read as an edit.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	bool UploadThresholds();
	void UpdateClock();
	void UpdateAudio();

	ffglex::FFGLShader separateShader;
	ffglex::FFGLShader printShader;
	ffglex::FFGLScreenQuad quad;

	rosette::PassBuffer platesBuffer;///< cmyk * alpha, mipmapped

	GLuint thresholdTexture = 0;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live by the fleet), the
	// offline harness sends seconds. steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names
	// the unit: 1 for seconds, 1000 for milliseconds. Several frames vote so
	// a single odd frame cannot decide it alone. Copied from tinsel.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	/// The clock everything reads, in seconds, after the unit is settled.
	double now     = 0.0;
	double lastNow = -1.0;

	//---------------------------------------------------------------------
	// Audio.
	//---------------------------------------------------------------------
	rosette::audio::Analyser analyser;
	float kickDirection[ 4 ][ 2 ] = {};
	unsigned int kickSalt         = 0;

	/// Where each plate was put on the last frame.
	float plateOffset[ 4 ][ 2 ] = {};

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
