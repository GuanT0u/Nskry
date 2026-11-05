import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { z } from 'astro/zod';
const schema = z.object({ title:z.string(), description:z.string().optional(), order:z.number().optional() });
export const collections = {
  guides:defineCollection({ loader:glob({pattern:'**/*.md',base:'./src/content/guides'}), schema }),
  plugins:defineCollection({ loader:glob({pattern:'**/*.md',base:'./src/content/plugins'}), schema }),
  releases:defineCollection({ loader:glob({pattern:'**/*.md',base:'./src/content/releases'}), schema }),
};
