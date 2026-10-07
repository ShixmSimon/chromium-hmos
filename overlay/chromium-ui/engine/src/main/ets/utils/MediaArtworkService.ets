import { common } from '@kit.AbilityKit';
import { fileIo, fileUri } from '@kit.CoreFileKit';
import { SystemServiceHandler } from './SystemServiceBridge';

class ArtworkLocation {
  directory: string = '';
  uriDirectory: string = '';
}

let artworkLocation: Promise<ArtworkLocation> | undefined = undefined;

async function initializeArtworkCache(context: common.UIAbilityContext): Promise<ArtworkLocation> {
  try {
    const directory = `${context.cacheDir}/chromium-media-artwork`;
    if (!await fileIo.access(directory)) {
      await fileIo.mkdir(directory, true);
    }
    const names = await fileIo.listFile(directory);
    for (const name of names) {
      if (/^artwork-[a-fA-F0-9]+\.png$/.test(name)) {
        await fileIo.unlink(`${directory}/${name}`);
      }
    }
    const location = new ArtworkLocation();
    const applicationUri = fileUri.getUriFromPath(directory);
    const pathStart = applicationUri.indexOf('/', 'file://'.length);
    if (!applicationUri.startsWith('file://') || pathStart < 0) {
      return location;
    }
    location.directory = directory;
    location.uriDirectory = `file://${applicationUri.substring(pathStart)}`;
    return location;
  } catch {
    return new ArtworkLocation();
  }
}

export class MediaArtworkService implements SystemServiceHandler {
  constructor(context: common.UIAbilityContext) {
    if (!artworkLocation) {
      artworkLocation = initializeArtworkCache(context);
    }
  }

  async handle(method: string, _args: Record<string, Object>): Promise<Object | undefined> {
    if (method !== 'location' || !artworkLocation) {
      throw new Error('Media artwork location unavailable');
    }
    return artworkLocation;
  }
}
