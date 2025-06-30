import "package:whisper_flutter_acft/whisper_flutter_acft.dart";

class TranscribeResult {
  const TranscribeResult({
    required this.transcription,
    required this.time,
  });

  final WhisperTranscribeResponse transcription;
  final Duration time;
}
