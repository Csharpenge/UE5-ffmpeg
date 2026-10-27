// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;
using UnrealBuildTool;

/**
 * FFmpeg linkage policy for this project:
 *
 *   Android                          -> bundled libraries under ThirdParty/ffmpeg (include + lib/Android/<abi>).
 *   Win64 / Linux / LinuxArm64 / Mac -> system FFmpeg development installation.
 *
 * The system prefix is resolved in this order:
 *   1. UFFMPEG_SYSTEM_FFMPEG_ROOT (must contain include/libavcodec/avcodec.h)
 *   2. Windows: %VCPKG_ROOT%/installed/x64-windows (also VCPKG_INSTALLATION_ROOT, then common paths)
 *      Linux:   /usr/local, /usr
 *      macOS:   /opt/homebrew, /usr/local
 *
 * The FFmpeg version is read from the resolved headers, so 4.x / 6.x / 8.x / 9.x are all supported
 * by the compatibility layer in Public/FFmpegCompat.h.
 */
public class UFFmpeg : ModuleRules
{
	private string ModulePath
	{
		get { return ModuleDirectory; }
	}

	private string ThirdPartyPath
	{
		get { return Path.GetFullPath(Path.Combine(ModulePath, "../../ThirdParty")); }
	}

	private string FFmpegRoot
	{
		get { return Path.Combine(ThirdPartyPath, "ffmpeg"); }
	}

	/** Linker library names (MSVC import libs drop the "lib" prefix as well). */
	private static readonly string[] FFmpegLibraries =
	{
		"avfilter",
		"avformat",
		"avcodec",
		"swresample",
		"swscale",
		"avutil"
	};

	public UFFmpeg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			ConfigureBundledAndroid(Target);
		}
		else
		{
			ConfigureSystemFFmpeg(Target);
		}

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"AudioMixer",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"Projects",
				"RHI",
				"RHICore",
				"RenderCore",
			});

		OptimizeCode = CodeOptimization.InShippingBuildsOnly;
	}

	/** Android: the only platform that uses the vendored FFmpeg copy. */
	private void ConfigureBundledAndroid(ReadOnlyTargetRules Target)
	{
		string Include = Path.Combine(FFmpegRoot, "include");
		if (!Directory.Exists(Include))
		{
			throw new BuildException("UFFmpeg: bundled FFmpeg headers not found at '" + Include + "'.");
		}

		PublicIncludePaths.Add(Include);

		VersionNumbers Version = VersionNumbers.Read(Include);
		Console.WriteLine("[UFFmpeg] bundled FFmpeg (Android): avcodec " + Version.AvCodec + ", avformat " + Version.AvFormat + ", avutil " + Version.AvUtil);

		PublicDefinitions.Add("UFFMPEG_SYSTEM_FFMPEG=0");
		PublicDefinitions.Add("UFFMPEG_SYSTEM_ROOT=\"\"");

		string Architecture = Target.Architecture.ToString();
		string Abi = Architecture.Contains("Arm64", StringComparison.OrdinalIgnoreCase) ? "arm64-v8a"
			: (Architecture.Contains("X64", StringComparison.OrdinalIgnoreCase) ? "x86_64" : "armeabi-v7a");

		LinkStatic(Path.Combine(FFmpegRoot, "lib", "Android", Abi), "Android/" + Abi);

		// The bundled Android FFmpeg is configured with --enable-libx264, so the final link also
		// needs the x264 static library (it is an external dependency, not part of libavcodec).
		string X264Library = Path.Combine(FFmpegRoot, "lib", "Android", Abi, "libx264.a");
		if (File.Exists(X264Library))
		{
			PublicAdditionalLibraries.Add(X264Library);
		}
		else
		{
			Console.WriteLine("[UFFmpeg] WARNING: " + X264Library + " not found. The bundled FFmpeg was built with --enable-libx264, linking will fail without it.");
		}

		PublicSystemLibraries.AddRange(new string[] { "m", "z", "log" });
	}

	/** Win64 / Linux / LinuxArm64 / Mac: link the FFmpeg installed on the build machine. */
	private void ConfigureSystemFFmpeg(ReadOnlyTargetRules Target)
	{
		string Root = ResolveSystemRoot(Target);
		if (string.IsNullOrEmpty(Root))
		{
			throw new BuildException(
				"UFFmpeg: no system FFmpeg found for " + Target.Platform + ".\n" +
				"  Windows : vcpkg install ffmpeg[core,avcodec,avformat,avfilter,swscale,swresample]:x64-windows (or set VCPKG_ROOT)\n" +
				"  Linux   : apt install libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev libswscale-dev libswresample-dev\n" +
				"  macOS   : brew install ffmpeg\n" +
				"  Or point UFFMPEG_SYSTEM_FFMPEG_ROOT at an FFmpeg prefix containing include/ and lib/.\n" +
				"  Android does not need a system FFmpeg, it always uses ThirdParty/ffmpeg.");
		}

		string Include = Path.Combine(Root, "include");
		if (!Directory.Exists(Include))
		{
			throw new BuildException("UFFmpeg: system FFmpeg prefix '" + Root + "' has no include directory.");
		}

		PublicIncludePaths.Add(Include);

		VersionNumbers Version = VersionNumbers.Read(Include);
		Console.WriteLine("[UFFmpeg] system FFmpeg (" + Target.Platform + "): avcodec " + Version.AvCodec + ", avformat " + Version.AvFormat + ", avutil " + Version.AvUtil + ", avfilter " + Version.AvFilter + ", swscale " + Version.SwScale + ", swresample " + Version.SwResample + " from " + Root);

		PublicDefinitions.Add("UFFMPEG_SYSTEM_FFMPEG=1");
		PublicDefinitions.Add("UFFMPEG_SYSTEM_ROOT=\"" + Root.Replace('\\', '/') + "\"");

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			LinkWindows(Path.Combine(Root, "lib"), Path.Combine(Root, "bin"), Version);
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.LinuxArm64)
		{
			LinkLinux(Root);
			PublicSystemLibraries.AddRange(new string[] { "m", "pthread", "dl" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			LinkMac(Root);
		}
		else
		{
			throw new BuildException("UFFmpeg: unsupported platform '" + Target.Platform + "'. Supported: Win64, Linux, LinuxArm64, Android, Mac.");
		}
	}

	/** Finds an FFmpeg prefix that contains include/libavcodec/avcodec.h. */
	private string ResolveSystemRoot(ReadOnlyTargetRules Target)
	{
		string FromEnvironment = Environment.GetEnvironmentVariable("UFFMPEG_SYSTEM_FFMPEG_ROOT");
		if (!string.IsNullOrEmpty(FromEnvironment))
		{
			if (HasHeaders(FromEnvironment))
			{
				return FromEnvironment.TrimEnd('\\', '/');
			}
			Console.WriteLine("[UFFmpeg] WARNING: UFFMPEG_SYSTEM_FFMPEG_ROOT='" + FromEnvironment + "' has no include/libavcodec/avcodec.h, ignoring it.");
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			foreach (string Variable in new string[] { "VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT" })
			{
				string VcpkgRoot = Environment.GetEnvironmentVariable(Variable);
				if (string.IsNullOrEmpty(VcpkgRoot))
				{
					continue;
				}

				foreach (string Triplet in new string[] { "x64-windows", "x64-windows-static-md" })
				{
					string Candidate = Path.Combine(VcpkgRoot, "installed", Triplet);
					if (HasHeaders(Candidate))
					{
						return Candidate;
					}
				}
			}

			foreach (string Candidate in new string[]
			{
				@"C:\vcpkg\installed\x64-windows",
				@"C:\WorkTool\vcpkg\installed\x64-windows",
				@"C:\ffmpeg"
			})
			{
				if (HasHeaders(Candidate))
				{
					return Candidate;
				}
			}
			return null;
		}

		if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.LinuxArm64)
		{
			foreach (string Candidate in new string[] { "/usr/local", "/usr" })
			{
				if (HasHeaders(Candidate))
				{
					return Candidate;
				}
			}
			return null;
		}

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			foreach (string Candidate in new string[] { "/opt/homebrew", "/usr/local" })
			{
				if (HasHeaders(Candidate))
				{
					return Candidate;
				}
			}
			return null;
		}

		return null;
	}

	private static bool HasHeaders(string Prefix)
	{
		return !string.IsNullOrEmpty(Prefix) && File.Exists(Path.Combine(Prefix, "include", "libavcodec", "avcodec.h"));
	}

	/** Windows: import libraries from <prefix>/lib, runtime DLLs from <prefix>/bin. */
	private void LinkWindows(string LibraryDirectory, string BinaryDirectory, VersionNumbers Version)
	{
		foreach (string Library in FFmpegLibraries)
		{
			string LibraryPath = Path.Combine(LibraryDirectory, Library + ".lib");
			if (File.Exists(LibraryPath))
			{
				PublicAdditionalLibraries.Add(LibraryPath);
			}
			else
			{
				Console.WriteLine("[UFFmpeg] WARNING: missing FFmpeg import library: " + LibraryPath);
			}
		}

		string[] Dlls = new string[]
		{
			"avfilter-" + Version.AvFilter + ".dll",
			"avformat-" + Version.AvFormat + ".dll",
			"avcodec-" + Version.AvCodec + ".dll",
			"swresample-" + Version.SwResample + ".dll",
			"swscale-" + Version.SwScale + ".dll",
			"avutil-" + Version.AvUtil + ".dll"
		};

		foreach (string Dll in Dlls)
		{
			PublicDelayLoadDLLs.Add(Dll);

			if (!File.Exists(Path.Combine(BinaryDirectory, Dll)))
			{
				Console.WriteLine("[UFFmpeg] WARNING: FFmpeg runtime DLL not found: " + Path.Combine(BinaryDirectory, Dll));
			}
		}
	}

	/** Linux: links libav*.so (or .a) from <prefix>/lib*. For /usr the default linker search paths are used. */
	private void LinkLinux(string Prefix)
	{
		bool bStandardPrefix = Prefix == "/usr" || Prefix == "/usr/local";

		string[] LibraryDirectories = new string[]
		{
			Path.Combine(Prefix, "lib"),
			Path.Combine(Prefix, "lib64"),
			Path.Combine(Prefix, "lib", "x86_64-linux-gnu"),
			Path.Combine(Prefix, "lib", "aarch64-linux-gnu")
		};

		foreach (string Library in FFmpegLibraries)
		{
			if (!bStandardPrefix)
			{
				string Resolved = null;
				foreach (string Directory in LibraryDirectories)
				{
					foreach (string Extension in new string[] { ".so", ".a" })
					{
						string Candidate = Path.Combine(Directory, "lib" + Library + Extension);
						if (File.Exists(Candidate))
						{
							Resolved = Candidate;
							break;
						}
					}
					if (Resolved != null)
					{
						break;
					}
				}

				if (Resolved != null)
				{
					PublicAdditionalLibraries.Add(Resolved);
					continue;
				}
			}

			// Standard prefix (or unresolved custom prefix): let the linker search for -lavcodec etc.
			PublicSystemLibraries.Add(Library);
		}
	}

	/** macOS: framework style dylibs from the Homebrew prefix. */
	private void LinkMac(string Prefix)
	{
		string LibraryDirectory = Path.Combine(Prefix, "lib");

		foreach (string Library in FFmpegLibraries)
		{
			string Link = Path.Combine(LibraryDirectory, "lib" + Library + ".dylib");
			if (File.Exists(Link))
			{
				PublicAdditionalLibraries.Add(Link);
				continue;
			}

			string[] Versioned = Directory.Exists(LibraryDirectory) ? Directory.GetFiles(LibraryDirectory, "lib" + Library + ".*.dylib") : new string[0];
			if (Versioned.Length > 0)
			{
				Array.Sort(Versioned, StringComparer.OrdinalIgnoreCase);
				PublicAdditionalLibraries.Add(Versioned[Versioned.Length - 1]);
			}
			else
			{
				Console.WriteLine("[UFFmpeg] WARNING: missing FFmpeg library in " + LibraryDirectory + " for " + Library);
			}
		}
	}

	private void LinkStatic(string LibraryDirectory, string PlatformName)
	{
		if (!Directory.Exists(LibraryDirectory))
		{
			Console.WriteLine("[UFFmpeg] WARNING: bundled FFmpeg libraries for " + PlatformName + " not found (expected in " + LibraryDirectory + "). Linking will fail until they are added.");
			return;
		}

		foreach (string Library in FFmpegLibraries)
		{
			string LibraryPath = Path.Combine(LibraryDirectory, "lib" + Library + ".a");
			if (File.Exists(LibraryPath))
			{
				PublicAdditionalLibraries.Add(LibraryPath);
			}
			else
			{
				Console.WriteLine("[UFFmpeg] WARNING: missing FFmpeg static library: " + LibraryPath);
			}
		}
	}

	/** FFmpeg library major versions, read from the headers the module is compiled against. */
	private struct VersionNumbers
	{
		public int AvCodec;
		public int AvFormat;
		public int AvUtil;
		public int AvFilter;
		public int SwScale;
		public int SwResample;

		public static VersionNumbers Read(string IncludeDirectory)
		{
			VersionNumbers Result = new VersionNumbers();
			Result.AvCodec = ReadMajor(IncludeDirectory, "libavcodec", "LIBAVCODEC_VERSION_MAJOR");
			Result.AvFormat = ReadMajor(IncludeDirectory, "libavformat", "LIBAVFORMAT_VERSION_MAJOR");
			Result.AvUtil = ReadMajor(IncludeDirectory, "libavutil", "LIBAVUTIL_VERSION_MAJOR");
			Result.AvFilter = ReadMajor(IncludeDirectory, "libavfilter", "LIBAVFILTER_VERSION_MAJOR");
			Result.SwScale = ReadMajor(IncludeDirectory, "libswscale", "LIBSWSCALE_VERSION_MAJOR");
			Result.SwResample = ReadMajor(IncludeDirectory, "libswresample", "LIBSWRESAMPLE_VERSION_MAJOR");

			if (Result.AvCodec <= 0 || Result.AvFormat <= 0 || Result.AvUtil <= 0)
			{
				throw new BuildException("UFFmpeg: unable to read the FFmpeg version macros from '" + IncludeDirectory + "'.");
			}
			return Result;
		}

		private static int ReadMajor(string IncludeDirectory, string Library, string Macro)
		{
			string[] Candidates = new string[]
			{
				Path.Combine(IncludeDirectory, Library, "version_major.h"),
				Path.Combine(IncludeDirectory, Library, "version.h")
			};

			foreach (string Candidate in Candidates)
			{
				if (!File.Exists(Candidate))
				{
					continue;
				}

				foreach (string RawLine in File.ReadAllLines(Candidate))
				{
					string Line = RawLine.Trim();
					if (!Line.StartsWith("#define "))
					{
						continue;
					}

					string[] Parts = Line.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
					if (Parts.Length >= 3 && Parts[1] == Macro)
					{
						int Value;
						if (int.TryParse(Parts[2], out Value))
						{
							return Value;
						}
					}
				}
			}
			return 0;
		}
	}
}
