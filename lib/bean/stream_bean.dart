import 'dart:convert';
import 'dart:typed_data';

import 'package:whisper_flutter_acft/bean/whisper_dto.dart';

// --- Request classes ---

class StreamInitRequest implements WhisperRequestDto {
  final String model;
  final String language;
  final int threads;
  final String initialPrompt;

  StreamInitRequest({
    required this.model,
    this.language = "auto",
    this.threads = 6,
    this.initialPrompt = "",
  });

  @override
  String get specialType => "streamInit";

  @override
  String toRequestString() {
    return json.encode({
      "@type": specialType,
      "model": model,
      "language": language,
      "threads": threads,
      "initial_prompt": initialPrompt,
    });
  }
}

class StreamProcessRequest implements WhisperRequestDto {
  final int sessionId;
  final Float32List audioData;

  StreamProcessRequest({
    required this.sessionId,
    required this.audioData,
  });

  @override
  String get specialType => "streamProcess";

  @override
  String toRequestString() {
    final bytes = audioData.buffer.asUint8List(
      audioData.offsetInBytes,
      audioData.lengthInBytes,
    );
    final encoded = base64Encode(bytes);
    return json.encode({
      "@type": specialType,
      "session_id": sessionId,
      "audio_data": encoded,
    });
  }
}

class StreamFinalizeRequest implements WhisperRequestDto {
  final int sessionId;

  StreamFinalizeRequest({required this.sessionId});

  @override
  String get specialType => "streamFinalize";

  @override
  String toRequestString() {
    return json.encode({
      "@type": specialType,
      "session_id": sessionId,
    });
  }
}

class StreamCancelRequest implements WhisperRequestDto {
  final int sessionId;

  StreamCancelRequest({required this.sessionId});

  @override
  String get specialType => "streamCancel";

  @override
  String toRequestString() {
    return json.encode({
      "@type": specialType,
      "session_id": sessionId,
    });
  }
}

// --- Response classes ---

class StreamInitResponse {
  final int sessionId;

  StreamInitResponse({required this.sessionId});

  factory StreamInitResponse.fromJson(Map<String, dynamic> json) {
    return StreamInitResponse(sessionId: json["session_id"] as int);
  }
}

class StreamProcessResponse {
  final String text;
  final String committedText;

  StreamProcessResponse({required this.text, required this.committedText});

  factory StreamProcessResponse.fromJson(Map<String, dynamic> json) {
    return StreamProcessResponse(
      text: json["text"] as String? ?? "",
      committedText: json["committed_text"] as String? ?? "",
    );
  }
}

class StreamFinalizeResponse {
  final String text;

  StreamFinalizeResponse({required this.text});

  factory StreamFinalizeResponse.fromJson(Map<String, dynamic> json) {
    return StreamFinalizeResponse(text: json["text"] as String? ?? "");
  }
}
